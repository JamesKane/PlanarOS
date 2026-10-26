/*
 * servers/workbench/main.c -- the Workbench: the desktop's backdrop, the
 * volumes as icons, and a window for each volume or drawer opened
 * (V8 step 7; docs §13.5)
 *
 * hwdb starts it on the desktop, with intuition, and keeps it as it
 * keeps intuition, until it Quits. It opens a borderless WFLG_BACKDROP
 * window under the screen bar, the whole screen's width, kept behind
 * every other, and draws the backdrop and the volumes dos lists there
 * (wb.h). A click on an icon selects it; two within WB_DOUBLE_NS open
 * the volume: a window of its entries (a ui.library list, wb.h's), from
 * which a drawer's double click opens another, and a file's starts it
 * as a program with no arguments, its directory its current one.
 *
 * Its menus, on every window of its own: Workbench (About, Execute
 * Command..., Quit), Window (Open Parent, Close), Tools (Shell, Prefs).
 * Execute Command takes a line in a small window and runs it as the
 * Shell would: its first word a command in C: (or a path), the rest its
 * arguments, SYS: its directory. Shell and Prefs are hwdb's to start
 * (HWDB_NEW_SHELL, HWDB_NEW_PREFS), so they outlive the Workbench.
 *
 * intuition's settings (GetPrefs) say the backdrop's colours and whether
 * the HUD shows: a UI_HudBadge at the top right, the CPU and memory from
 * QuerySystem once a second, on the backdrop by ui.library's backfill. A
 * change comes as IDCMP_NEWPREFS.
 *
 * Every window has one buffer: before an event that may draw, the last
 * frame must be safe. The backdrop's drawn once into a bitmap of its own
 * and copied from there. A program the Workbench starts is its child,
 * and ends with it. If intuition goes, the Workbench ends, and hwdb
 * starts it again, to open its windows on the next.
 */
#include <stddef.h>

#include <planar/dos.h>
#include <planar/drv.h>
#include <planar/intuition.h>
#include <planar/syscalls.h>
#include <planar/ui.h>
#include <planar/utility.h>

#include "log.h"
#include "wb.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_DOS = 16, SIG_CHILD = 17, SIG_CALL = 18;  /* 22 is intuition.library's */
constexpr uint64_t MS = 1000000, SECOND = 1000 * MS;
constexpr unsigned DRAWERS = 6;             /* windows of volumes and drawers, at most */
constexpr unsigned CHILDREN = 8;            /* programs started, at most */
constexpr uint32_t EXEC_W = 380, EXEC_H = 104;  /* Execute Command's window, inside */
constexpr unsigned EXEC_LINE = 160;

/* The menus, every window's; their numbers as MENUPICK has them. */
static const struct NewMenu menus[] = {
    { NM_TITLE, "Workbench", nullptr, 0 },
    { NM_ITEM, "About", nullptr, 0 },
    { NM_ITEM, "Execute Command...", "E", 0 },
    { NM_ITEM, NM_BARLABEL, nullptr, 0 },
    { NM_ITEM, "Quit", "Q", 0 },
    { NM_TITLE, "Window", nullptr, 0 },
    { NM_ITEM, "Open Parent", nullptr, 0 },
    { NM_ITEM, "Close", "K", 0 },
    { NM_TITLE, "Tools", nullptr, 0 },
    { NM_ITEM, "Shell", nullptr, 0 },
    { NM_ITEM, "Prefs", nullptr, 0 },
    { NM_END, nullptr, nullptr, 0 },
};
enum { M_WORKBENCH, M_WINDOW, M_TOOLS };
enum { I_ABOUT = 0, I_EXECUTE = 1, I_QUIT = 3, I_PARENT = 0, I_CLOSE = 1, I_SHELL = 0, I_PREFS = 1 };

/* Execute Command's gadgets. */
enum : uint32_t { X_ROOT = 1, X_LABEL, X_FIELD, X_BUTTONS, X_CANCEL, X_RUN };

struct drawer {
    bool open;
    struct Window w;
    struct BitMap *bm;
    struct UI ui;
    char path[DOS_PATH];
    char title[INTUI_TITLE];
    struct wb_drawer d;
};

static struct {
    struct GfxIFace *gfx;
    struct IntuitionIFace *intuition;
    struct UIIFace *iui;
    struct Intuition in;
    struct Dos dos;
    Handle task;
    struct TextFont *sans;
    struct IntuiPrefs prefs;

    /* The backdrop window: drawn from 'cache', the backdrop alone. */
    struct Window back;
    struct BitMap *bm, *cache;
    uint32_t w, h;
    struct wb_icon icon[WB_VOLUMES];
    unsigned nicons;
    int click_icon;                     /* the last click's, for a double; -1 none */
    uint64_t click_time;

    /* The HUD, on the backdrop: a UI of its own, its root the backfill. */
    struct UI hud;
    bool hud_on;
    char hud_text[32];
    uint64_t hud_next, hud_idle, hud_at;

    struct drawer dr[DRAWERS];
    unsigned opened;                    /* drawers opened, ever: where the next goes */

    /* Execute Command's window, while it's open. */
    bool exec_open;
    struct Window xw;
    struct BitMap *xbm;
    struct UI xui;
    char line[EXEC_LINE];

    Handle child[CHILDREN];
} wb = { .click_icon = -1 };

static void say(const char *s)
{
    KLog(s);
}

/* --- Drawing. --- */

static void present(struct Window *w, struct UI *ui)
{
    if (ui->ui_NumDamage && wb.intuition->PresentWindow(w, ui->ui_Damage, ui->ui_NumDamage) == ERR_PORT_DIED)
        ExitProcess(3);
    ui->ui_NumDamage = 0;
}

static void safe(struct Window *w)
{
    if (wb.intuition->WaitSafe(w, CurrentTime() + SECOND) == ERR_PORT_DIED)
        ExitProcess(3);
}

/* The icons that meet r, over the cache's backdrop there. */
static void backdrop_part(struct RastPort *rp, struct GfxRect r)
{
    struct GfxRect saved = rp->rp_Clip;

    wb.gfx->BltBitMap(wb.cache, r.MinX, r.MinY, rp, r.MinX, r.MinY, r.MaxX - r.MinX + 1, r.MaxY - r.MinY + 1,
                      BLEND_SRC);
    rp->rp_Clip = (struct GfxRect){ saved.MinX > r.MinX ? saved.MinX : r.MinX, saved.MinY > r.MinY ? saved.MinY : r.MinY,
                                    saved.MaxX < r.MaxX ? saved.MaxX : r.MaxX, saved.MaxY < r.MaxY ? saved.MaxY : r.MaxY };
    for (unsigned i = 0; i < wb.nicons; i++) {
        struct GfxRect ir = wb_icon_rect(wb.gfx, rp, wb.sans, &wb.icon[i]);

        if (ir.MaxX >= r.MinX && ir.MinX <= r.MaxX && ir.MaxY >= r.MinY && ir.MinY <= r.MaxY)
            wb_icon_draw(wb.gfx, rp, wb.sans, &wb.icon[i]);
    }
    rp->rp_Clip = saved;
}

static void backfill(struct UI *ui, struct GfxRect r)
{
    backdrop_part(ui->ui_RPort, r);
}

/* The whole backdrop window, from the cache drawn again. */
static void backdrop_all(void)
{
    struct RastPort rp;

    wb.gfx->InitRastPort(&rp, wb.cache);
    wb_backdrop(wb.gfx, &rp, wb.w, wb.h, wb.prefs.ip_BackdropTop, wb.prefs.ip_BackdropBottom);
    safe(&wb.back);
    if (wb.hud_on) {
        wb.iui->DrawUI(&wb.hud);
        wb.hud.ui_NumDamage = 0;
    } else {
        backdrop_part(&wb.back.w_RPort, (struct GfxRect){ 0, 0, (int32_t)wb.w - 1, (int32_t)wb.h - 1 });
    }
    if (wb.intuition->PresentWindow(&wb.back, nullptr, 0) == ERR_PORT_DIED)
        ExitProcess(3);
}

/* Icon i drawn again as it is now. */
static void icon_redraw(int i)
{
    struct GfxRect r;

    if (i < 0)
        return;
    r = wb_icon_rect(wb.gfx, &wb.back.w_RPort, wb.sans, &wb.icon[i]);
    r.MinY -= 4;                        /* the lid, lifted */
    safe(&wb.back);
    backdrop_part(&wb.back.w_RPort, r);
    if (wb.intuition->PresentWindow(&wb.back, &r, 1) == ERR_PORT_DIED)
        ExitProcess(3);
}

/* --- The HUD. --- */

static void hud_read(void)
{
    struct SystemInfo si = { 0 };
    uint64_t now = CurrentTime(), busy = 0, used = 0;
    char cpu[8], ram[8];

    QuerySystem(&si);
    if (wb.hud_at && now > wb.hud_at && si.si_CPUs) {
        uint64_t all = (now - wb.hud_at) * si.si_CPUs, idle = si.si_IdleNanos - wb.hud_idle;

        busy = idle >= all ? 0 : (all - idle) * 1000 / all;
    }
    if (si.si_Pages)
        used = (si.si_Pages - si.si_FreePages) * 1000 / si.si_Pages;
    wb.hud_at = now;
    wb.hud_idle = si.si_IdleNanos;
    SNPrintf(cpu, sizeof cpu, "%lu.%lu", busy / 10, busy % 10);
    SNPrintf(ram, sizeof ram, "%lu.%lu", used / 10, used % 10);
    SNPrintf(wb.hud_text, sizeof wb.hud_text, "CPU %6s%%\nRAM %6s%%", cpu, ram);
}

static void hud_tick(void)
{
    if (!wb.hud_on || CurrentTime() < wb.hud_next)
        return;
    wb.hud_next = CurrentTime() + SECOND;
    hud_read();
    safe(&wb.back);
    SetGadgetAttrs(wb.iui, &wb.hud, 2, { UIA_Label, (uint64_t)wb.hud_text });
    present(&wb.back, &wb.hud);
}

/* --- Starting things. --- */

static int64_t hwdb_call(uint32_t op)
{
    struct MsgPort mp;
    struct HwdbMsg *m;
    int64_t e;

    if ((e = ConnectPort(&mp, "hwdb", SIG_CALL, 2, 2 * sizeof *m)) != 0)
        return e;
    e = ERR_NOMEM;
    if ((m = AllocMsg(&mp, sizeof *m))) {
        memset(m, 0, sizeof *m);
        m->hm_Msg.mn_Length = sizeof *m;
        m->hm_Op = op;
        if ((e = DoMsgUntil(&mp, &m->hm_Msg, CurrentTime() + DRV_HWDB_MS * MS)) == 0)
            e = m->hm_Msg.mn_Error;
    }
    ClosePort(&mp);
    return e;
}

static void tell(struct Window *w, const char *title, const char *body)
{
    struct EasyStruct es = { sizeof es, 0, title, body, "OK" };

    wb.intuition->EasyRequest(&wb.in, w, &es);
}

/* Those that ended let go of. */
static void children(void)
{
    struct ProcessInfo pi;

    for (unsigned i = 0; i < CHILDREN; i++)
        if (wb.child[i] && QueryProcess(wb.child[i], &pi) == 0 && pi.pi_State >= PROCS_EXITED) {
            CloseHandle(wb.child[i]);
            wb.child[i] = 0;
        }
}

/* A program, its command line 'args' (its directory, a newline, its arguments). */
static int64_t start(const char *path, const char *args)
{
    unsigned i;
    int64_t p;

    children();
    for (i = 0; i < CHILDREN && wb.child[i]; i++)
        ;
    if (i == CHILDREN)
        return ERR_FULL;
    if ((p = RunCommand(&wb.dos, path, args, SIG_CHILD, 0)) < 0)
        return p;
    wb.child[i] = (Handle)p;
    return 0;
}

/* --- Drawers. --- */

/* Two paths the same, in any case (dos's names are). */
static bool same_path(const char *a, const char *b)
{
    while (*a && (*a | 0x20) == (*b | 0x20))
        a++, b++;
    return !*a && !*b;
}

static struct drawer *drawer_of(uint32_t id)
{
    for (unsigned i = 0; i < DRAWERS; i++)
        if (wb.dr[i].open && wb.dr[i].w.w_Id == id)
            return &wb.dr[i];
    return nullptr;
}

static void drawer_close(struct drawer *d)
{
    wb.intuition->CloseWindow(&d->w);
    wb.iui->FreeUI(&d->ui);
    wb.gfx->FreeBitMap(d->bm);
    d->open = false;
}

/* A window of path's entries: true if it opened. */
static bool drawer_open(struct Window *from, const char *path)
{
    struct drawer *d = nullptr;
    const char *name;
    int32_t left, top;
    int64_t e;

    for (unsigned i = 0; i < DRAWERS; i++)
        if (wb.dr[i].open && same_path(wb.dr[i].path, path))
            return true;                /* open already */
    for (unsigned i = 0; i < DRAWERS && !d; i++)
        if (!wb.dr[i].open)
            d = &wb.dr[i];
    if (!d) {
        tell(from, "Workbench", "Too many windows are open.\nClose one first.");
        return false;
    }
    memset(d, 0, offsetof(struct drawer, d));
    SNPrintf(d->path, sizeof d->path, "%s", path);
    if ((e = wb_read(&wb.dos, path, &d->d)) != 0) {
        char body[DOS_PATH + 32];

        SNPrintf(body, sizeof body, "%s couldn't be read (%ld).", path, e);
        tell(from, "Workbench", body);
        return false;
    }
    name = path;
    for (const char *p = path; *p; p++)
        if ((*p == ':' || *p == '/') && p[1])
            name = p + 1;
    SNPrintf(d->title, sizeof d->title, "%s", name);
    if (!(d->bm = wb.gfx->AllocBitMap(WB_DRAWER_W, WB_DRAWER_H, PIXFMT_XRGB8888, 0, nullptr)))
        return false;
    if (wb.iui->InitUI(&d->ui, wb.gfx) != 0 || !wb_drawer_build(wb.iui, &d->ui, &d->d)) {
        wb.gfx->FreeBitMap(d->bm);
        return false;
    }
    wb_drawer_at(wb.opened, &left, &top);
    e = OpenWindowTags(wb.intuition, &wb.in, &d->w, { WA_Left, (uint64_t)left }, { WA_Top, (uint64_t)top },
                       { WA_Width, WB_DRAWER_W }, { WA_Height, WB_DRAWER_H }, { WA_Title, (uint64_t)d->title },
                       { WA_Flags, WFLG_SIZEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                                   WFLG_ACTIVATE },
                       { WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY | IDCMP_RAWKEY |
                                   IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE | IDCMP_MENUPICK },
                       { WA_MinWidth, 160 }, { WA_MinHeight, 120 }, { WA_BitMap, (uint64_t)d->bm });
    if (e != 0) {
        wb.iui->FreeUI(&d->ui);
        wb.gfx->FreeBitMap(d->bm);
        return false;
    }
    d->open = true;
    wb.opened++;
    wb.intuition->SetMenuStrip(&d->w, menus);
    d->ui.ui_RPort = &d->w.w_RPort;
    wb.iui->LayoutUI(&d->ui, d->w.w_Width, d->w.w_Height);
    wb.iui->DrawUI(&d->ui);
    d->ui.ui_NumDamage = 0;
    if (wb.intuition->PresentWindow(&d->w, nullptr, 0) == ERR_PORT_DIED)
        ExitProcess(3);
    return true;
}

/* A drawer window's row double clicked: a drawer opened, a file started. */
static void drawer_choose(struct drawer *d, int32_t row)
{
    char path[DOS_PATH], args[DOS_PATH + 2];
    uint64_t n;
    int64_t e;

    if (row < 0 || (uint32_t)row >= d->d.count)
        return;
    SNPrintf(path, sizeof path, "%s", d->path);
    if (!AddPart(path, d->d.label[row], sizeof path))
        return;
    n = wb_len(path);
    if (d->d.dir[row] && n && path[n - 1] == '/')
        path[n - 1] = 0;                /* the label's '/' */
    if (d->d.dir[row]) {
        drawer_open(&d->w, path);
        return;
    }
    SNPrintf(args, sizeof args, "%s\n", d->path);
    if ((e = start(path, args)) != 0) {
        char body[DOS_PATH + 48];

        SNPrintf(body, sizeof body, "%s %s.", d->d.label[row],
                 e == ERR_BADARG ? "isn't a program this machine runs" : "couldn't be started");
        tell(&d->w, "Workbench", body);
    }
}

static void drawer_resize(struct drawer *d, uint32_t w, uint32_t h)
{
    struct BitMap *bm = wb.gfx->AllocBitMap(w, h, PIXFMT_XRGB8888, 0, nullptr);

    if (!bm)
        return;
    if (wb.intuition->ChangeWindowBuffers(&d->w, bm, nullptr) != 0) {
        wb.gfx->FreeBitMap(bm);
        return;
    }
    wb.gfx->FreeBitMap(d->bm);
    d->bm = bm;
    d->ui.ui_RPort = &d->w.w_RPort;
    wb.iui->LayoutUI(&d->ui, w, h);
    wb.iui->DrawUI(&d->ui);
    d->ui.ui_NumDamage = 0;
    if (wb.intuition->PresentWindow(&d->w, nullptr, 0) == ERR_PORT_DIED)
        ExitProcess(3);
}

/* Window: Open Parent. */
static void parent(struct drawer *d)
{
    char path[DOS_PATH];
    char *cut;

    SNPrintf(path, sizeof path, "%s", d->path);
    cut = PathPart(path);
    if (cut == path + wb_len(path) || (cut > path && cut[-1] == ':' && !*cut))
        return;                         /* a volume: no parent */
    *cut = 0;
    drawer_open(&d->w, path);
}

/* --- Execute Command. --- */

static void exec_close(void)
{
    if (!wb.exec_open)
        return;
    wb.intuition->CloseWindow(&wb.xw);
    wb.iui->FreeUI(&wb.xui);
    wb.gfx->FreeBitMap(wb.xbm);
    wb.exec_open = false;
}

static void exec_open(void)
{
    uint32_t n = 0;

    if (wb.exec_open)
        return;
    wb.line[0] = 0;
    if (!(wb.xbm = wb.gfx->AllocBitMap(EXEC_W, EXEC_H, PIXFMT_XRGB8888, 0, nullptr)))
        return;
    if (wb.iui->InitUI(&wb.xui, wb.gfx) != 0) {
        wb.gfx->FreeBitMap(wb.xbm);
        return;
    }
    n += AddGadgetTags(wb.iui, &wb.xui, 0, UI_COLUMN, { UIA_Margin, 12 }, { UIA_Spacing, 8 }) == X_ROOT;
    n += AddGadgetTags(wb.iui, &wb.xui, X_ROOT, UI_LABEL, { UIA_Label, (uint64_t)"Enter Command and its Arguments:" }) ==
         X_LABEL;
    n += AddGadgetTags(wb.iui, &wb.xui, X_ROOT, UI_TEXTFIELD, { UIA_Buffer, (uint64_t)wb.line },
                       { UIA_Size, EXEC_LINE }) == X_FIELD;
    n += AddGadgetTags(wb.iui, &wb.xui, X_ROOT, UI_ROW, { UIA_Align, UIALIGN_END }) == X_BUTTONS;
    n += AddGadgetTags(wb.iui, &wb.xui, X_BUTTONS, UI_BUTTON, { UIA_Label, (uint64_t)"Cancel" }) == X_CANCEL;
    n += AddGadgetTags(wb.iui, &wb.xui, X_BUTTONS, UI_BUTTON, { UIA_Label, (uint64_t)"Run" }) == X_RUN;
    if (n != X_RUN ||
        OpenWindowTags(wb.intuition, &wb.in, &wb.xw, { WA_Left, (uint64_t)(wb.w / 2 - EXEC_W / 2) },
                       { WA_Top, 160 }, { WA_Title, (uint64_t)"Execute Command" },
                       { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE },
                       { WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY | IDCMP_RAWKEY |
                                   IDCMP_CLOSEWINDOW | IDCMP_MENUPICK },
                       { WA_BitMap, (uint64_t)wb.xbm }) != 0) {
        wb.iui->FreeUI(&wb.xui);
        wb.gfx->FreeBitMap(wb.xbm);
        return;
    }
    wb.exec_open = true;
    wb.intuition->SetMenuStrip(&wb.xw, menus);
    wb.xui.ui_RPort = &wb.xw.w_RPort;
    wb.xui.ui_Focus = X_FIELD;
    wb.iui->LayoutUI(&wb.xui, EXEC_W, EXEC_H);
    wb.iui->DrawUI(&wb.xui);
    wb.xui.ui_NumDamage = 0;
    if (wb.intuition->PresentWindow(&wb.xw, nullptr, 0) == ERR_PORT_DIED)
        ExitProcess(3);
}

/* The line run as the Shell runs one: its first word the command, in
 * C: unless it names a volume, the rest its arguments, SYS: its
 * directory. */
static void exec_run(void)
{
    char path[DOS_PATH], args[EXEC_LINE + 8];
    const char *s = wb.line, *rest;
    unsigned n = 0;
    bool volume = false;
    int64_t e;

    while (*s == ' ')
        s++;
    if (!*s)
        return;
    for (rest = s; *rest && *rest != ' '; rest++)
        volume |= *rest == ':';
    n = (unsigned)(rest - s);
    SNPrintf(path, sizeof path, "%s%.*s", volume ? "" : "C:", n, s);
    while (*rest == ' ')
        rest++;
    SNPrintf(args, sizeof args, "SYS:\n%s", rest);
    exec_close();
    if ((e = start(path, args)) != 0) {
        char body[DOS_PATH + 48];

        SNPrintf(body, sizeof body, "%s %s (%ld).", path,
                 e == ERR_NOTFOUND ? "isn't there" : e == ERR_BADARG ? "isn't a program" : "couldn't be run", e);
        tell(nullptr, "Execute Command", body);
    }
}

/* --- Menus. --- */

static void about(struct Window *w)
{
    tell(w, "About the Workbench",
         "The Workbench, V8.\nThe volumes dos lists, as icons; each volume\nor drawer opened, a window of its entries.");
}

/* True to keep going; false: Quit. */
static bool menu(struct Window *w, struct drawer *d, uint16_t code)
{
    struct EasyStruct quit = { sizeof quit, 0, "Workbench", "Quit the Workbench?\nIts windows close.", "Quit|Cancel" };
    unsigned m = MENUNUM(code), i = ITEMNUM(code);
    int64_t e;

    if (code == MENUNULL)
        return true;
    if (m == M_WORKBENCH && i == I_ABOUT)
        about(w);
    else if (m == M_WORKBENCH && i == I_EXECUTE)
        exec_open();
    else if (m == M_WORKBENCH && i == I_QUIT)
        return wb.intuition->EasyRequest(&wb.in, w, &quit) != 1;
    else if (m == M_WINDOW && i == I_PARENT && d)
        parent(d);
    else if (m == M_WINDOW && i == I_CLOSE && d)
        drawer_close(d);
    else if (m == M_WINDOW && i == I_CLOSE && w == &wb.xw)
        exec_close();
    else if (m == M_TOOLS && i == I_SHELL && (e = hwdb_call(HWDB_NEW_SHELL)) != 0)
        KPrintF("workbench: hwdb wouldn't start a Shell in a window (%ld)", e);
    else if (m == M_TOOLS && i == I_PREFS && (e = hwdb_call(HWDB_NEW_PREFS)) != 0 && e != ERR_EXISTS)
        KPrintF("workbench: hwdb wouldn't start Prefs (%ld)", e);
    return true;
}

/* --- Events. --- */

static int icon_at(int32_t x, int32_t y)
{
    for (unsigned i = 0; i < wb.nicons; i++) {
        struct GfxRect r = wb_icon_rect(wb.gfx, &wb.back.w_RPort, wb.sans, &wb.icon[i]);

        if (x >= r.MinX && x <= r.MaxX && y >= r.MinY && y <= r.MaxY)
            return (int)i;
    }
    return -1;
}

static void backdrop_click(const struct IntuiEvent *ev)
{
    int i = icon_at(ev->iev_MouseX, ev->iev_MouseY);
    bool twice = i >= 0 && i == wb.click_icon && ev->iev_Time - wb.click_time < WB_DOUBLE_NS;

    for (unsigned k = 0; k < wb.nicons; k++)
        if (wb.icon[k].selected != ((int)k == i)) {
            wb.icon[k].selected = (int)k == i;
            icon_redraw((int)k);
        }
    wb.click_icon = twice ? -1 : i;
    wb.click_time = ev->iev_Time;
    if (twice) {
        char path[WB_NAME + 1];

        SNPrintf(path, sizeof path, "%s:", wb.icon[i].name);
        drawer_open(&wb.back, path);
    }
}

static void prefs_changed(void)
{
    struct IntuiPrefs p;
    bool hud, colours;

    if (wb.intuition->GetPrefs(&wb.in, &p) != 0)
        return;
    hud = p.ip_Hud != wb.prefs.ip_Hud;
    colours = p.ip_BackdropTop != wb.prefs.ip_BackdropTop || p.ip_BackdropBottom != wb.prefs.ip_BackdropBottom;
    wb.prefs = p;
    wb.hud_on = p.ip_Hud;
    if (wb.hud_on && hud) {
        hud_read();
        wb.hud_next = CurrentTime() + SECOND;
        SetGadgetAttrs(wb.iui, &wb.hud, 2, { UIA_Label, (uint64_t)wb.hud_text });
        wb.iui->LayoutUI(&wb.hud, wb.w, wb.h);
    }
    if (hud || colours)
        backdrop_all();
}

/* Every event come: false once the Workbench is to Quit. */
static bool events(void)
{
    struct IntuiEvent ev;
    struct UIChange c;

    while (wb.intuition->GetIntuiEvent(&wb.in, &ev)) {
        struct drawer *d = drawer_of(ev.iev_Window);
        struct Window *w = d ? &d->w : ev.iev_Window == wb.back.w_Id ? &wb.back
                                     : wb.exec_open && ev.iev_Window == wb.xw.w_Id ? &wb.xw : nullptr;

        if (!w)
            continue;
        if (ev.iev_Class == IDCMP_MENUPICK) {
            if (!menu(w, d, ev.iev_Code))
                return false;
            continue;
        }
        if (w == &wb.back) {
            if (ev.iev_Class == IDCMP_MOUSEBUTTONS && ev.iev_Code == SELECTDOWN)
                backdrop_click(&ev);
            else if (ev.iev_Class == IDCMP_NEWPREFS)
                prefs_changed();
            continue;
        }
        if (ev.iev_Class == IDCMP_CLOSEWINDOW) {
            if (d)
                drawer_close(d);
            else
                exec_close();
            continue;
        }
        if (d && ev.iev_Class == IDCMP_NEWSIZE) {
            drawer_resize(d, (uint32_t)ev.iev_MouseX, (uint32_t)ev.iev_MouseY);
            continue;
        }
        safe(w);
        if (d) {
            if (wb.iui->HandleUIEvent(&d->ui, &ev, &c) == WB_D_LIST && c.uc_Flags & UICF_DOUBLE) {
                present(w, &d->ui);
                drawer_choose(d, c.uc_Value);
                continue;
            }
            present(w, &d->ui);
        } else {
            uint32_t g = wb.iui->HandleUIEvent(&wb.xui, &ev, &c);

            present(w, &wb.xui);
            if (g == X_RUN || (g == X_FIELD && c.uc_Flags & UICF_ENTER))
                exec_run();
            else if (g == X_CANCEL)
                exec_close();
        }
    }
    return true;
}

/* --- Starting. --- */

static bool open_libs(void)
{
    struct Library *lib;

    return (lib = IExec->OpenLibrary("gfx.library", 8)) &&
           (wb.gfx = (struct GfxIFace *)IExec->GetInterface(lib, "main", 1)) && wb.gfx->OpenTextFont &&
           (lib = IExec->OpenLibrary("intuition.library", 8)) &&
           (wb.intuition = (struct IntuitionIFace *)IExec->GetInterface(lib, "main", 1)) &&
           wb.intuition->Data.if_Size >= offsetof(struct IntuitionIFace, SetPrefs) + sizeof(void *) &&
           (lib = IExec->OpenLibrary("ui.library", 8)) &&
           (wb.iui = (struct UIIFace *)IExec->GetInterface(lib, "main", 1)) &&
           (wb.sans = wb.gfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 }));
}

static bool backdrop_up(void)
{
    int64_t e;

    wb.w = wb.in.in_Width;
    wb.h = wb.in.in_Height - INTUI_SCREENBAR;
    if (!(wb.bm = wb.gfx->AllocBitMap(wb.w, wb.h, PIXFMT_XRGB8888, 0, nullptr)) ||
        !(wb.cache = wb.gfx->AllocBitMap(wb.w, wb.h, PIXFMT_XRGB8888, 0, nullptr)))
        return false;
    e = OpenWindowTags(wb.intuition, &wb.in, &wb.back, { WA_Left, 0 }, { WA_Top, INTUI_SCREENBAR },
                       { WA_Width, wb.w }, { WA_Height, wb.h }, { WA_Title, (uint64_t)"Workbench" },
                       { WA_Flags, WFLG_BORDERLESS | WFLG_BACKDROP | WFLG_ACTIVATE },
                       { WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MENUPICK | IDCMP_NEWPREFS },
                       { WA_BitMap, (uint64_t)wb.bm });
    if (e != 0) {
        KPrintF("workbench: no window on intuition (%ld)", e);
        return false;
    }
    wb.intuition->SetMenuStrip(&wb.back, menus);
    if (wb.iui->InitUI(&wb.hud, wb.gfx) != 0)
        return false;
    wb.hud.ui_Backfill = backfill;
    wb.hud.ui_RPort = &wb.back.w_RPort;
    SNPrintf(wb.hud_text, sizeof wb.hud_text, "CPU %6s%%\nRAM %6s%%", "0.0", "0.0");
    AddGadgetTags(wb.iui, &wb.hud, 0, UI_COLUMN, { UIA_Pane, UIPANE_BACKFILL }, { UIA_Margin, 16 });
    AddGadgetTags(wb.iui, &wb.hud, 1, UI_HUDBADGE, { UIA_Label, (uint64_t)wb.hud_text }, { UIA_Align, UIALIGN_END });
    wb.iui->LayoutUI(&wb.hud, wb.w, wb.h);
    wb.hud_on = wb.prefs.ip_Hud;
    if (wb.hud_on)
        hud_read();
    backdrop_all();
    return true;
}

int main(uint64_t)
{
    int64_t e;

    wb.task = FindTask(nullptr);
    if (!open_libs()) {
        say("workbench: no gfx.library, intuition.library 8.7, ui.library or System Sans");
        return 1;
    }
    if ((e = OpenDosUntil(&wb.dos, SIG_DOS, CurrentTime() + 10000 * MS)) != 0) {
        KPrintF("workbench: no dos (%ld)", e);
        return 1;
    }
    if ((e = wb.intuition->OpenIntuition(&wb.in, CurrentTime() + 10000 * MS)) != 0) {
        KPrintF("workbench: no intuition (%ld)", e);
        return 1;
    }
    if (wb.intuition->GetPrefs(&wb.in, &wb.prefs) != 0)
        wb.prefs = (struct IntuiPrefs){ 0, INTUI_POINTER_AUTO, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM };
    wb.nicons = wb_volumes(&wb.dos, wb.icon, WB_VOLUMES);
    if (!backdrop_up())
        return 1;
    KPrintF("workbench: the backdrop, %lux%lu, and %lu volumes' icons", (uint64_t)wb.w, (uint64_t)wb.h,
            (uint64_t)wb.nicons);
    for (;;) {
        uint64_t until = wb.hud_on ? wb.hud_next : CurrentTime() + SECOND;

        e = wb.intuition->WaitIntuiEvent(&wb.in, until);
        if (e == ERR_PORT_DIED || PortGone(&wb.in.in_Port)) {
            say("workbench: intuition went: ending, for hwdb to start again");
            return 3;
        }
        if (!events())
            break;
        hud_tick();
        children();
    }
    for (unsigned i = 0; i < DRAWERS; i++)
        if (wb.dr[i].open)
            drawer_close(&wb.dr[i]);
    exec_close();
    wb.intuition->CloseWindow(&wb.back);
    say("workbench: quit");
    return 0;
}
