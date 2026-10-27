/*
 * conformance/window.c -- intuition's windows, presented, worked by the
 * mouse and keys, and outliving the servers (v7-plan steps 3 to 5; V8
 * step 4: menus, a resize and requesters)
 *
 * Started by hwdb's window test (servers/hwdb/intuition.c) once it has
 * started intuition. Through intuition.library, with bitmaps from
 * gfx.library:
 *
 * 1. Two windows opened, borderless, overlapping: one double-buffered,
 *    one not. Each presented whole, then the first in part (its back
 *    buffer brought up to date first), and the screen read back
 *    (DISPLAY_READ, where display composes it) against the windows as
 *    this program drew them over the backdrop, and the pointer. The
 *    second shows typescene.h's type, paths and gradients (V8.3).
 * 2. The refusals: a window wider or taller than its buffer, a buffer
 *    that says it's bigger than its Area, a buffer not shared, damage
 *    outside the window, a frame not above the last, a buffer that isn't
 *    one, and a window that isn't the caller's (from a second
 *    connection). Then a window off the screen, whose frames are safe
 *    and shown though they change nothing there.
 * 3. FRAMES frames of the first window presented as fast as is_Safe
 *    allows: the frames waited for (each a kernel call; one whose
 *    buffer was free cost none), the vblanks the screen missed meanwhile,
 *    and the machine's kernel calls a frame. Then frames presented one at
 *    a time, from PresentWindow to is_DispTime.
 *    Then PACED frames each presented just after a vblank, of the first
 *    window and of one the screen's size, timed as displaytest times its
 *    flips: present to latch, vblank to latch, and the vblanks missed,
 *    which must be none on a board (v7-plan step 6). PACED_READ of each
 *    on QEMU, said but not checked: its display composes the planes in
 *    software as it latches, the whole screen for intuition's plane.
 * 4. Both closed, and the backdrop read back where they were.
 * 5. The mouse and keys, which it asks for ("window: send ..."), as
 *    inputtest does: on QEMU the boot check sends them through QMP.
 *    Two windows with chrome: focus moved by a click, each event where
 *    it should be and the window clicked brought to the front; keys at
 *    the active window alone; a window dragged by its title bar, sent
 *    back and brought forward by its depth gadget, and closed by its
 *    close gadget's IDCMP_CLOSEWINDOW. The chrome isn't read back, only
 *    the windows' insides and the backdrop; nor the screen bar.
 * 6. A client that has stopped (this program, started again with
 *    STOPPED): its window dragged and raised all the same, and a second
 *    after its close gadget, which it never answers, intuition asks
 *    whether to close it. Close: hwdb ends it, and its window goes.
 * 6a. V8 step 4, with the mouse: a window with a menu strip, a menu
 *    opened by a click on its title in the screen bar (drawn over the
 *    window, as read back) and an item chosen; another held open by the
 *    right button, a CHECKIT item checked; one chosen by its key with
 *    the GUI key; the system menu's About, and its requester's OK. The
 *    window resized by its size gadget: told the new size, its old
 *    buffer shown and the backdrop past it, then new buffers given and
 *    the old ones let go of; and held to its least size. An EasyRequest
 *    from a second task, answered by a click on its middle button.
 * 6b. V8 step 5: the system menu's Shell, a console window (conwin) with
 *    a Shell in it; its inside read back with the prompt up, then a
 *    command typed (shellwin.h's), a script run by a second Shell on the
 *    same console, and read back again, each against the host's CRC
 *    (servers/conwin/test.c). EndShell typed: the window goes. Then a
 *    conwin of this program's, killed once its prompt is up: its window
 *    gone by the next latch but one, and its console with it.
 * 6c. V8 step 6: ui.library's gadgets, every one, in a window of this
 *    program's (uiwin.h's), read back as the host draws it; then clicked,
 *    dragged and typed through as uiwin.h's script says, each event into
 *    HandleUIEvent and its damage presented, each step's change as the
 *    host's (lib/ui/test.c), and read back again.
 * 6d. V8 step 7: the Workbench (a child of this program's), its backdrop
 *    and dos's volumes' icons read back as wb.h draws them; the first
 *    volume opened by a double click, its window's list read back as
 *    wb.h draws what dos lists there (as List lists it), and a drawer in
 *    it opened from its list the same way. Then Tools' Prefs from the
 *    screen bar's menu (hwdb starts it): its repeat rate and pointer
 *    speed set by clicks on their sliders (each value as this program's
 *    copy of the window takes the same click), input's INPUT_GET
 *    agreeing and a move scaled by the speed; the HUD turned on, and
 *    intuition's GetPrefs agreeing; Save, and the files read back. The
 *    pointer's speed put back, and the Workbench ended.
 * 7. kill-window: a client (this program again, KILLED) with two windows
 *    over one of this one's, killed. They're gone from the screen by the
 *    next latch but one, and intuition lets go of their bitmaps, which
 *    the client shared with this program too to show it: then they're
 *    this program's alone, and freed when it lets go.
 *
 * Then the costs, intuition's own: composing, a megapixel. Exits 0;
 * NO_INPUT if 1 to 4 and 7 passed and nobody moved the mouse (a board
 * with nobody at it), or intuition has no input; or the failed check's
 * number.
 *
 * 8. With SURVIVE, hwdb's second round, across the servers' deaths: two
 *    windows checked, then a third opened, hwdb's cue to kill intuition.
 *    The ring's wait returns ERR_PORT_DIED, and the console shows where
 *    the windows were. Connected again as soon as intuition's back, the
 *    windows are opened again from the same bitmaps, with nothing of the
 *    old connection left mapped; the third is the cue again, to kill
 *    display. Frames presented while it's away are safe at once, and
 *    shown once it's back. Then input is killed: its ring is opened
 *    again, the pointer where it was, and (SURVIVE_MOUSE, if 5 ran) the
 *    settings 6d saved in force again, read by input and intuition at
 *    their starts, then put back to their defaults and the files
 *    removed; and a click heard after. Exits 0, or the failed check's number.
 *
 * 9. With DESKTOP, hwdb's desktop round (V8 step 8): intuition and the
 *    Workbench kept by hwdb as on the desktop, and killed under it in
 *    turn (HWDB_TEST_KILL), the screen read back all the while (where it
 *    can be) and never black. The Workbench, back with its icons. Prefs
 *    and a Shell's conwin, both hwdb's: the conwin killed, its window and
 *    con.1 gone; Prefs killed, its window gone and hwdb starting another.
 *    Settings saved, then intuition killed under the Workbench and Prefs:
 *    the console shows, the Workbench comes back on its next self in the
 *    saved backdrop's colours, and Prefs doesn't; input killed, and back
 *    with the saved settings. Then, with the mouse, a window dragged over
 *    the Workbench, its frames' cost; with DRAG, by hand (5's drag) over
 *    the Workbench. Exits 0, or the failed check's number.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/devices/input.h>
#include <planar/drv.h>
#include <planar/intuition.h>
#include <planar/prefs.h>
#include <planar/syscalls.h>
#include <planar/ui.h>

#include "../servers/conwin/conwin.h"
#include "../servers/intuition/pointer.h"
#include "../servers/prefs/prefswin.h"
#include "../servers/workbench/wb.h"
#include "log.h"
#include "shellwin.h"
#include "typescene.h"
#include "uiwin.h"

constexpr int64_t SIG_DISPLAY = 20, SIG_HUNG = 21, SIG_KILL = 23, SIG_CONWIN = 24;  /* 22 is intuition.library's */
constexpr int64_t SIG_DOS = 25, SIG_INPUT = 26, SIG_WB = 27, SIG_HWDB = 28;
constexpr uint64_t MS = 1000000;
constexpr unsigned FRAMES = 120, TIMED = 10;
constexpr unsigned PACED = 600, PACED_READ = 30;     /* a board's, 10 s at 60 Hz; QEMU's */
constexpr uint32_t BACKDROP = 0x182038;     /* intuition's (compose.h's IC_BACKDROP) */
constexpr uint32_t CHROME = 0x00c0ffee;     /* the model's: a pixel of the chrome, not read; clear, so no pixel drawn */
constexpr int NO_INPUT = 1000;
enum : uint64_t { FIRST, STOPPED, KILLED, SURVIVE, SURVIVE_MOUSE = 4, DESKTOP = 8 };     /* the argument's low byte */
constexpr uint64_t DESKTOP_MOUSE = 1u << 4;     /* with DESKTOP: the mouse is there (5 ran) */
constexpr uint64_t LOOK = 1u << 8;          /* hwdb's WINDOW_LOOK: hold the first windows on the screen */
constexpr uint64_t DRAG = 1u << 9;          /* hwdb's WINDOW_DRAG: windows to drag by hand */
constexpr unsigned DRAG_S = 300;            /* at most: until the close gadget */

/* The stopped client's window, and the killed one's two, which both ends know. */
constexpr int32_t HUNG_X = 420, HUNG_Y = 80;
constexpr uint32_t HUNG_W = 120, HUNG_H = 80, HUNG_COLOUR = 0x307050;
constexpr int32_t KILL_X = 180, KILL_Y = 280, KILL2_X = 60, KILL2_Y = 360;
constexpr uint32_t KILL_W = 200, KILL_H = 100, KILL_COLOUR = 0x705030;
constexpr uint32_t KILL2_W = 120, KILL2_H = 80, KILL2_COLOUR = 0x504070;

static unsigned step;

static int failed(const char *what)
{
    KPrintF("window: check %lu failed: %s", (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static struct GfxIFace *IGfx;
static struct IntuitionIFace *IIntuition;

/* --- display, for reading the screen back. --- */

static struct MsgPort dmp;
static struct DisplayMsg *dm;
static const struct DisplaySync *dsync;

static int64_t display(uint32_t op)
{
    dm->dm_Msg.mn_Length = sizeof *dm;
    dm->dm_Op = op;
    dm->dm_Task = FindTask(nullptr);
    if (DoMsgUntil(&dmp, &dm->dm_Msg, CurrentTime() + 1000 * MS) != 0)
        return ERR_PORT_DIED;
    return dm->dm_Msg.mn_Error;
}

static int64_t display_up(uint64_t deadline)
{
    int64_t e = ConnectPortUntil(&dmp, "display.0", SIG_DISPLAY, 2, 2 * sizeof *dm, deadline);

    if (e != 0)
        return e;
    if (!(dm = AllocMsg(&dmp, sizeof *dm))) {
        ClosePort(&dmp);
        return ERR_NOMEM;
    }
    memset(dm, 0, sizeof *dm);
    if ((e = display(DISPLAY_INFO)) != 0) {
        ClosePort(&dmp);
        return e;
    }
    dsync = (const struct DisplaySync *)dm->dm_Sync;
    return 0;
}

static int64_t read_screen(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t *crc)
{
    int64_t e;

    memset(dm, 0, sizeof *dm);
    dm->dm_Set[0] = (struct DisplayPlane){ .dp_X = x, .dp_Y = y };
    dm->dm_Width = w;
    dm->dm_Height = h;
    e = display(DISPLAY_READ);
    *crc = dm->dm_Crc;
    return e;
}

/* --- What the screen should show. --- */

static struct Intuition in;
static struct Window wa, wb;
static int32_t ptr_x, ptr_y;        /* the pointer, as intuition last said */
static uint32_t ptr_scale = 1;      /* its sprite's, from the screen's size (pointer.h) */

/* Another client's window, a flat colour: the stopped client's, and
 * the killed one's two. */
struct other {
    int32_t x, y;                   /* its top left, its chrome's if it has any */
    uint32_t w, h, colour;          /* its inside's */
    bool chrome;
};

static struct other others[3] = {
    { HUNG_X, HUNG_Y, HUNG_W, HUNG_H, HUNG_COLOUR, true },
    { KILL_X, KILL_Y, KILL_W, KILL_H, KILL_COLOUR, true },
    { KILL2_X, KILL2_Y, KILL2_W, KILL2_H, KILL2_COLOUR, false },
};

#define HUNG (&others[0])

/* The windows on the screen, front first: each a Window of this
 * program's, or an other. */
static const void *order[8];
static unsigned nshown;

static const struct other *other_of(const void *k)
{
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; i++)
        if (k == &others[i])
            return &others[i];
    return nullptr;
}

static void to_front(const void *k)
{
    unsigned i = 0;

    while (i < nshown && order[i] != k)
        i++;
    if (i == nshown)
        nshown++;
    for (; i > 0; i--)
        order[i] = order[i - 1];
    order[0] = k;
}

static void to_back(const void *k)
{
    unsigned i = 0;

    while (i < nshown && order[i] != k)
        i++;
    for (; i + 1 < nshown; i++)
        order[i] = order[i + 1];
    order[nshown - 1] = k;
}

static void taken_off(const void *k)
{
    unsigned i = 0;

    while (i < nshown && order[i] != k)
        i++;
    if (i == nshown)
        return;
    for (nshown--; i < nshown; i++)
        order[i] = order[i + 1];
}

/* Where a window is: its top left, its inside's offset from there, and
 * its inside's size. */
struct place {
    int32_t left, top, bl, bt, br, bb, iw, ih;
};

static struct place place_of(const void *k)
{
    const struct other *o = other_of(k);
    const struct Window *w = k;

    if (o)
        return (struct place){ o->x, o->y, o->chrome ? (int32_t)INTUI_BORDER : 0,
                               o->chrome ? (int32_t)INTUI_TITLEBAR : 0, o->chrome ? (int32_t)INTUI_BORDER : 0,
                               o->chrome ? (int32_t)INTUI_BORDER : 0, (int32_t)o->w, (int32_t)o->h };
    /* As resized: intuition's sync words have its inside's size. */
    return (struct place){ w->w_Sync->is_LeftEdge, w->w_Sync->is_TopEdge, w->w_BorderLeft,
                           w->w_BorderTop, w->w_BorderRight, w->w_BorderBottom,
                           (int32_t)w->w_Sync->is_Width, (int32_t)w->w_Sync->is_Height };
}

/* A window's inside at (x, y): its buffer shown last, and past it (a
 * window resized, not yet given new buffers) the backdrop. */
static uint32_t shown_px(const struct Window *w, int32_t x, int32_t y)
{
    const struct BitMap *bm = w->w_BitMap[(w->w_Back + w->w_Buffers - 1) % w->w_Buffers];

    if (x >= (int32_t)bm->bm_Width || y >= (int32_t)bm->bm_Height)
        return BACKDROP;
    return ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x];
}

/* (x, y) of the screen, from window 'k' of the model: false if it's not on it. */
static bool from(const void *k, int32_t x, int32_t y, uint32_t *px)
{
    struct place p = place_of(k);
    const struct other *o = other_of(k);

    if (x < p.left || y < p.top || x >= p.left + p.bl + p.iw + p.br || y >= p.top + p.bt + p.ih + p.bb)
        return false;
    x -= p.left + p.bl;
    y -= p.top + p.bt;
    if (x < 0 || y < 0 || x >= p.iw || y >= p.ih)
        *px = CHROME;
    else
        *px = o ? o->colour : shown_px(k, x, y);
    return true;
}

static uint32_t screen_px(int32_t x, int32_t y)
{
    uint32_t p;

    if (x >= ptr_x && y >= ptr_y &&
        (p = pointer_pixel((uint32_t)(x - ptr_x) / ptr_scale, (uint32_t)(y - ptr_y) / ptr_scale)) >> 24)
        return p;
    if (y < (int32_t)INTUI_SCREENBAR)
        return CHROME;                  /* the screen bar, in front of every window */
    for (unsigned i = 0; i < nshown; i++)
        if (from(order[i], x, y, &p))
            return p;
    return BACKDROP;
}

/* The checksum DISPLAY_READ would give; false if the rectangle holds
 * chrome, which the model doesn't know (a test's mistake). */
static uint32_t crc_step(uint32_t crc, uint32_t px);
static void nap(uint64_t ms);

static bool expect(int32_t x0, int32_t y0, uint32_t w, uint32_t h, uint32_t *sum)
{
    uint32_t crc = ~0u;

    for (int32_t y = y0; y < y0 + (int32_t)h; y++)
        for (int32_t x = x0; x < x0 + (int32_t)w; x++) {
            uint32_t px = screen_px(x, y);

            if (px == CHROME)
                return false;
            crc = crc_step(crc, px);
        }
    *sum = ~crc;
    return true;
}

/* The rectangle as the screen shows it, a second at most to get there:
 * true if it did, or the screen can't be read here. */
static bool screen_is(bool readable, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    uint64_t until = CurrentTime() + 1000 * MS;
    uint32_t want, got;

    if (!expect(x, y, w, h, &want))
        return false;
    if (!readable)
        return true;
    do
        if (read_screen(x, y, w, h, &got) == 0 && got == want)
            return true;
        else
            nap(5);                     /* leave the CPU to what's to draw it */
    while (CurrentTime() < until);
    KPrintF("window: the screen at (%ld, %ld), %lux%lu, the pointer at (%ld, %ld): %08lx, not %08lx",
            (int64_t)x, (int64_t)y, (uint64_t)w, (uint64_t)h, (int64_t)ptr_x, (int64_t)ptr_y,
            (uint64_t)got, (uint64_t)want);
    return false;
}

/* Until frame n of w is on the screen, a second at most. */
static bool on_screen(const struct Window *w, uint64_t n)
{
    uint64_t until = CurrentTime() + 1000 * MS, v;

    while ((v = __atomic_load_n(&w->w_Sync->is_Disp, __ATOMIC_ACQUIRE)) < n)
        if (WaitAddr(&w->w_Sync->is_Disp, v, until) == ERR_TIMEOUT)
            return w->w_Sync->is_Disp >= n;
    return true;
}

/* A window's picture: a panel, its bevel and its name. */
static void picture(struct RastPort *rp, uint32_t w, uint32_t h, uint32_t bg, const char *name)
{
    rp->rp_FgPen = 0xff000000 | bg;
    IGfx->RectFill(rp, 0, 0, (int32_t)w - 1, (int32_t)h - 1);
    IGfx->DrawBevel(rp, 0, 0, (int32_t)w - 1, (int32_t)h - 1, 0xff8bb4e8, 0xff162030);
    rp->rp_FgPen = 0xfff0f4fa;
    rp->rp_DrawMode = JAM1;
    IGfx->Text(rp, 8, 8, name, 8);
}

/* The changing part of wa: a bar, and frame n's number in it. */
static const struct GfxRect bar = { 16, 40, 111, 55 };

static void tick(struct RastPort *rp, uint64_t n)
{
    char s[8];

    for (uint64_t i = 0, v = n; i < 8; i++, v /= 10)
        s[7 - i] = (char)('0' + v % 10);
    rp->rp_FgPen = 0xff000000 | (uint32_t)(n * 0x10305) % 0xffffff;
    IGfx->RectFill(rp, bar.MinX, bar.MinY, bar.MaxX, bar.MaxY);
    rp->rp_FgPen = 0xffffffff;
    rp->rp_BgPen = rp->rp_FgPen ^ 0xffffff;
    rp->rp_DrawMode = JAM2;
    IGfx->Text(rp, bar.MinX + 16, bar.MinY + 4, s, 8);
}

static int open_lib(void)
{
    struct Library *lib;

    CHECK((lib = IExec->OpenLibrary("gfx.library", 7)));
    CHECK((IGfx = (struct GfxIFace *)IExec->GetInterface(lib, "main", 1)));
    CHECK((lib = IExec->OpenLibrary("intuition.library", 7)) && lib->lib_Version == 8);
    CHECK((IIntuition = (struct IntuitionIFace *)IExec->GetInterface(lib, "main", 1)));
    CHECK(IIntuition->Data.if_Size == sizeof(struct IntuitionIFace));
    return 0;
}

/* 1: two windows, drawn, presented and read back. */
static int two_windows(bool *readable, struct BitMap **bm)
{
    uint32_t crc;
    int64_t e;

    CHECK((bm[0] = IGfx->AllocBitMap(200, 120, PIXFMT_ARGB8888, 0, nullptr)) &&
          (bm[1] = IGfx->AllocBitMap(200, 120, PIXFMT_ARGB8888, 0, nullptr)) &&
          (bm[2] = IGfx->AllocBitMap(160, 100, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &wa, { WA_Left, 64 }, { WA_Top, 48 }, { WA_Title, (uint64_t)"wa" },
                         { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)bm[0] },
                         { WA_BackBitMap, (uint64_t)bm[1] }) == 0);
    CHECK(OpenWindowTags(IIntuition, &in, &wb, { WA_Left, 180 }, { WA_Top, 100 },
                         { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)bm[2] }) == 0);
    CHECK(wa.w_Width == 200 && wa.w_Height == 120 && wa.w_Buffers == 2 && wb.w_Buffers == 1);
    picture(&wa.w_RPort, 200, 120, 0x5b88c2, "window a");
    CHECK(tc_scene(IGfx, &wb.w_RPort));     /* V8.3: type, paths and gradients, read back */
    CHECK(IIntuition->PresentWindow(&wa, nullptr, 0) == 0);
    CHECK(IIntuition->PresentWindow(&wb, nullptr, 0) == 0);
    to_front(&wa);
    to_front(&wb);
    CHECK(wa.w_Frame == 1 && wa.w_Back == 1 && wb.w_Back == 0);
    CHECK(on_screen(&wa, 1) && on_screen(&wb, 1));
    CHECK(IIntuition->WaitSafe(&wa, CurrentTime() + 1000 * MS) == 0);
    e = read_screen(32, 32, 416, 240, &crc);
    *readable = e == 0;
    CHECK(e == 0 || e == ERR_NOSYS);
    CHECK(screen_is(*readable, 32, 32, 416, 240));
    CHECK(screen_is(*readable, ptr_x - 8, ptr_y - 8, 32, 32));

    /* wa's back buffer brought up to date, a part changed and said to be. */
    IGfx->BltBitMap(wa.w_BitMap[0], 0, 0, &wa.w_RPort, 0, 0, 200, 120, BLEND_SRC);
    tick(&wa.w_RPort, 1);
    CHECK(IIntuition->PresentWindow(&wa, &bar, 1) == 0);
    CHECK(on_screen(&wa, 2));
    CHECK(screen_is(*readable, 32, 32, 416, 240));
    return 0;
}

/* 2: what intuition refuses, asked straight, past the library's checks. */
static int64_t ask(struct Intuition *i, struct IntuiMsg *m, uint32_t op)
{
    m->im_Msg.mn_Length = sizeof *m;
    m->im_Op = op;
    m->im_Task = FindTask(nullptr);
    if (DoMsgUntil(&i->in_Port, &m->im_Msg, CurrentTime() + 1000 * MS) != 0)
        return ERR_PORT_DIED;
    return m->im_Msg.mn_Error;
}

static void buffer_of(struct IntuiMsg *m, const struct BitMap *bm)
{
    m->im_Buffers = 1;
    m->im_Width = bm->bm_Width;
    m->im_Height = bm->bm_Height;
    m->im_Buffer[0] = (struct IntuiBuffer){ (uint64_t)bm->bm_BaseAddr, bm->bm_BytesPerRow,
                                            bm->bm_PixelFormat, bm->bm_Width, bm->bm_Height };
}

static int refusals(struct BitMap *const *bm)
{
    struct IntuiMsg *m = in.in_Call;
    struct Intuition other;
    struct Window w;
    struct BitMap *alone;
    struct GfxRect r = { 0, 0, 200, 0 };

    CHECK(OpenWindowTags(IIntuition, &in, &w, { WA_Width, 161 }, { WA_BitMap, (uint64_t)bm[2] }) ==
          ERR_BADARG);
    CHECK(OpenWindowTags(IIntuition, &in, &w, { WA_Height, 101 }, { WA_BitMap, (uint64_t)bm[2] }) ==
          ERR_BADARG);
    memset(m, 0, sizeof *m);
    buffer_of(m, bm[2]);
    m->im_Buffer[0].ib_Height = 4000;               /* past its Area */
    CHECK(ask(&in, m, INTUI_OPEN) == ERR_BADARG);
    memset(m, 0, sizeof *m);
    buffer_of(m, bm[2]);
    m->im_Buffer[0].ib_BytesPerRow = 2;             /* not a bitmap */
    CHECK(ask(&in, m, INTUI_OPEN) == ERR_BADARG);
    CHECK((alone = IGfx->AllocBitMap(64, 64, PIXFMT_XRGB8888, 0, nullptr)));
    memset(m, 0, sizeof *m);
    buffer_of(m, alone);                            /* never shared */
    CHECK(ask(&in, m, INTUI_OPEN) == ERR_RIGHTS);
    memset(m, 0, sizeof *m);
    buffer_of(m, bm[2]);
    m->im_Buffers = 3;
    CHECK(ask(&in, m, INTUI_OPEN) == ERR_BADARG);
    IGfx->FreeBitMap(alone);

    CHECK(IIntuition->PresentWindow(&wa, &r, 1) == ERR_BADARG);    /* the library's check */
    memset(m, 0, sizeof *m);
    m->im_Window = wa.w_Id;
    m->im_Frame = wa.w_Frame + 1;
    m->im_Count = 1;
    m->im_Rect[0] = r;                              /* a column past the window */
    CHECK(ask(&in, m, INTUI_PRESENT) == ERR_BADARG);
    m->im_Rect[0] = (struct GfxRect){ 0, 0, 0, 0 };
    m->im_Frame = wa.w_Frame;                       /* not above the last */
    CHECK(ask(&in, m, INTUI_PRESENT) == ERR_BADARG);
    m->im_Frame = wa.w_Frame + 1;
    m->im_Index = 2;                                /* no such buffer */
    CHECK(ask(&in, m, INTUI_PRESENT) == ERR_BADARG);

    CHECK(IIntuition->OpenIntuition(&other, CurrentTime() + 1000 * MS) == 0);
    memset(other.in_Call, 0, sizeof *other.in_Call);
    other.in_Call->im_Window = wa.w_Id;
    other.in_Call->im_Frame = wa.w_Frame + 1;
    CHECK(ask(&other, other.in_Call, INTUI_PRESENT) == ERR_NOTFOUND);
    CHECK(ask(&other, other.in_Call, INTUI_CLOSE) == ERR_NOTFOUND);
    IIntuition->CloseIntuition(&other);

    /* A window wholly off the screen: its frames change nothing there,
     * and are safe and shown all the same. */
    CHECK(OpenWindowTags(IIntuition, &in, &w, { WA_Left, -1000 }, { WA_Top, 100 },
                         { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)bm[2] }) == 0);
    CHECK(IIntuition->PresentWindow(&w, nullptr, 0) == 0);
    CHECK(IIntuition->PresentWindow(&w, nullptr, 0) == 0);
    CHECK(IIntuition->WaitSafe(&w, CurrentTime() + 1000 * MS) == 0 && on_screen(&w, 2));
    CHECK(IIntuition->CloseWindow(&w) == 0);
    return 0;
}

/* 3: as fast as is_Safe allows, then one at a time. */
static int frames(bool readable)
{
    struct ProcessInfo p0, p1;
    uint64_t v0, d0, waits = 0, lat_lo = UINT64_MAX, lat_hi = 0, shown, vblanks, calls;

    CHECK(QueryProcess(0, &p0) == 0);
    v0 = dsync->ds_Vblank;
    d0 = wa.w_Sync->is_Disp;
    for (unsigned f = 0; f < FRAMES; f++) {
        if (__atomic_load_n(&wa.w_Sync->is_Safe, __ATOMIC_ACQUIRE) < wa.w_Frame) {
            waits++;
            CHECK(IIntuition->WaitSafe(&wa, CurrentTime() + 1000 * MS) == 0);
        }
        tick(&wa.w_RPort, wa.w_Frame + 1);
        CHECK(IIntuition->PresentWindow(&wa, &bar, 1) == 0);
    }
    CHECK(on_screen(&wa, wa.w_Frame));
    CHECK(QueryProcess(0, &p1) == 0);
    calls = p1.pi_Calls - p0.pi_Calls;
    shown = wa.w_Sync->is_Disp - d0;
    vblanks = dsync->ds_Vblank - v0;
    CHECK(screen_is(readable, 32, 32, 416, 240));
    for (unsigned f = 0; f < TIMED; f++) {
        uint64_t t;

        CHECK(IIntuition->WaitSafe(&wa, CurrentTime() + 1000 * MS) == 0);
        tick(&wa.w_RPort, wa.w_Frame + 1);
        t = CurrentTime();
        CHECK(IIntuition->PresentWindow(&wa, &bar, 1) == 0);
        CHECK(on_screen(&wa, wa.w_Frame));
        /* On the screen, so composed: the back buffer is free, for no call
         * but QueryProcess's own. */
        CHECK(QueryProcess(0, &p0) == 0 && IIntuition->WaitSafe(&wa, 0) == 0 &&
              QueryProcess(0, &p1) == 0 && p1.pi_Calls - p0.pi_Calls == 1);
        t = wa.w_Sync->is_DispTime > t ? wa.w_Sync->is_DispTime - t : 0;
        lat_lo = t < lat_lo ? t : lat_lo;
        lat_hi = t > lat_hi ? t : lat_hi;
    }
    KPrintF("window: %lu frames as fast as is_Safe allowed: %lu shown in %lu vblanks; %lu waited "
            "for is_Safe, %lu didn't; %lu.%02lu kernel calls a frame",
            (uint64_t)FRAMES, shown, vblanks, waits, FRAMES - waits,
            calls / FRAMES, calls * 100 / FRAMES % 100);
    KPrintF("window: present to latch %lu-%lu us, over %lu frames one at a time", lat_lo / 1000,
            lat_hi / 1000, (uint64_t)TIMED);
    return 0;
}

static uint64_t last_vblank(uint64_t *at)
{
    uint64_t v;

    do {
        v = dsync->ds_Vblank;
        *at = dsync->ds_Time;
    } while (dsync->ds_Vblank != v);
    return v;
}

/* n frames of 'w', each presented whole just after a vblank: when each
 * latched (is_DispTime, the vblank's ds_Time), and the vblanks missed,
 * by the vblanks' period over the run. None may be on a board. */
static int paced(struct Window *w, unsigned n, bool readable, const char *what)
{
    static uint64_t late[PACED];        /* vblank to latch */
    uint64_t present_lo = UINT64_MAX, present_hi = 0, vblank_lo = UINT64_MAX, vblank_hi = 0, missed = 0;
    uint64_t v_first = 0, t_first = 0, v0 = 0, t0 = 0, period;

    for (unsigned f = 0; f < n; f++) {
        uint64_t t, t1, until = CurrentTime() + 250 * MS;

        CHECK(IIntuition->WaitSafe(w, CurrentTime() + 1000 * MS) == 0);
        tick(&w->w_RPort, w->w_Frame + 1);
        v0 = dsync->ds_Vblank;
        while (dsync->ds_Vblank == v0)
            CHECK(WaitAddr(&dsync->ds_Vblank, v0, until) != ERR_TIMEOUT || dsync->ds_Vblank != v0);
        v0 = last_vblank(&t0);
        if (f == 0) {
            v_first = v0;
            t_first = t0;
        }
        t = CurrentTime();
        CHECK(IIntuition->PresentWindow(w, nullptr, 0) == 0);
        CHECK(on_screen(w, w->w_Frame));
        t1 = w->w_Sync->is_DispTime;
        CHECK(t1 > t0);                 /* latched at a vblank after the present */
        late[f] = t1 - t0;
        present_lo = t1 - t < present_lo ? t1 - t : present_lo;
        present_hi = t1 - t > present_hi ? t1 - t : present_hi;
        vblank_lo = late[f] < vblank_lo ? late[f] : vblank_lo;
        vblank_hi = late[f] > vblank_hi ? late[f] : vblank_hi;
    }
    CHECK(v0 > v_first);
    period = (t0 - t_first) / (v0 - v_first);
    for (unsigned f = 0; f < n; f++) {
        uint64_t k = (late[f] + period / 2) / period;

        missed += k > 1 ? k - 1 : 0;
    }
    KPrintF("window: %lu frames of %s paced by the vblank: present to latch %lu-%lu us, vblank to latch "
            "%lu-%lu us, %lu vblanks missed",
            (uint64_t)n, what, present_lo / 1000, present_hi / 1000, vblank_lo / 1000, vblank_hi / 1000, missed);
    CHECK(readable || missed == 0);     /* QEMU's display composes plane 1 in software as it latches */
    return 0;
}

/* The first window's, then one the screen's size, two-buffered, over
 * everything until it's closed. */
static int paced_both(bool readable)
{
    unsigned n = readable ? PACED_READ : PACED;
    struct BitMap *fb[2] = { nullptr, nullptr };
    struct Window wf;
    int r;

    if ((r = paced(&wa, n, readable, "a 200x120 window")))
        return r;
    CHECK((fb[0] = IGfx->AllocBitMap(in.in_Width, in.in_Height, PIXFMT_XRGB8888, 0, nullptr)) &&
          (fb[1] = IGfx->AllocBitMap(in.in_Width, in.in_Height, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &wf, { WA_Left, 0 }, { WA_Top, 0 }, { WA_Flags, WFLG_BORDERLESS },
                         { WA_BitMap, (uint64_t)fb[0] }, { WA_BackBitMap, (uint64_t)fb[1] }) == 0);
    for (unsigned b = 0; b < 2; b++) {
        struct RastPort rp;

        IGfx->InitRastPort(&rp, fb[b]);
        picture(&rp, in.in_Width, in.in_Height, 0x5b88c2, "the whole screen");
    }
    if ((r = paced(&wf, n, readable, "a window the screen's size")))
        return r;
    CHECK(IIntuition->CloseWindow(&wf) == 0);
    CHECK(IIntuition->WaitSafe(&wa, CurrentTime() + 1000 * MS) == 0);
    IGfx->FreeBitMap(fb[0]);
    IGfx->FreeBitMap(fb[1]);
    return 0;
}

/* --- DRAG: windows dragged by hand, for someone at the board. --- */

static int64_t info(struct IntuiMsg *m);

/* Vertical stripes 16 wide, moved 4 right a frame: a tear shows as a
 * stripe broken across. */
static void stripes(struct RastPort *rp, uint32_t w, uint32_t h, uint64_t f)
{
    char s[8];

    for (int32_t x = -(int32_t)(f * 4 % 32); x < (int32_t)w; x += 16) {
        rp->rp_FgPen = (x + (int32_t)(f * 4 % 32)) & 16 ? 0xffe8e0d0 : 0xff203048;
        IGfx->RectFill(rp, x < 0 ? 0 : x, 0, x + 15 >= (int32_t)w ? (int32_t)w - 1 : x + 15, (int32_t)h - 1);
    }
    for (uint64_t i = 0, v = f; i < 8; i++, v /= 10)
        s[7 - i] = (char)('0' + v % 10);
    rp->rp_FgPen = 0xffffffff;
    rp->rp_BgPen = 0xff000000;
    rp->rp_DrawMode = JAM2;
    IGfx->Text(rp, 8, 8, s, 8);
}

/* A window's moves, one frame of the first window to the next. */
struct moves {
    int32_t x, y;
    uint64_t frames, dist;          /* frames moved in, and how far (|dx| + |dy|) */
    uint64_t stalls;                /* still for one frame between two moving */
    bool moved, was_moved;
};

static void sample(struct moves *m, const struct Window *w)
{
    int32_t x = w->w_Sync->is_LeftEdge, y = w->w_Sync->is_TopEdge;
    bool moved = x != m->x || y != m->y;

    if (moved) {
        m->frames++;
        m->dist += (uint64_t)(x > m->x ? x - m->x : m->x - x) + (uint64_t)(y > m->y ? y - m->y : m->y - y);
        if (!m->moved && m->was_moved)
            m->stalls++;
    }
    m->was_moved = m->moved;
    m->moved = moved;
    m->x = x;
    m->y = y;
}

/* Two windows with chrome until either's close gadget, or DRAG_S
 * seconds: the first 640x480, two-buffered, a frame of its
 * stripes presented as soon as the last is on the screen; the second
 * still. At each latch, where each is (is_LeftEdge, is_TopEdge); after,
 * the vblanks the first's frames missed, by the latches' times and the
 * vblanks' period. Said, not checked: a hand isn't a test. */
static int by_hand(void)
{
    static uint64_t gap[DRAG_S * 64];   /* latch to latch */
    const uint32_t chrome = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET;
    struct BitMap *b[3];
    struct Window wm, ws;
    struct moves mm = { 0 }, ms = { 0 };
    struct IntuiEvent ev;
    uint64_t end, say, frames = 0, missed = 0, last, v0, t0, v1, t1, period, moves0, nanos0;
    bool closed = false;

    CHECK((b[0] = IGfx->AllocBitMap(640, 480, PIXFMT_XRGB8888, 0, nullptr)) &&
          (b[1] = IGfx->AllocBitMap(640, 480, PIXFMT_XRGB8888, 0, nullptr)) &&
          (b[2] = IGfx->AllocBitMap(320, 200, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &wm, { WA_Left, 200 }, { WA_Top, 150 }, { WA_Title, (uint64_t)"drag me" },
                         { WA_Flags, chrome }, { WA_IDCMP, IDCMP_CLOSEWINDOW }, { WA_BitMap, (uint64_t)b[0] }, { WA_BackBitMap, (uint64_t)b[1] }) == 0);
    CHECK(OpenWindowTags(IIntuition, &in, &ws, { WA_Left, 1000 }, { WA_Top, 300 },
                         { WA_Title, (uint64_t)"close me to end" }, { WA_Flags, chrome | WFLG_ACTIVATE },
                         { WA_IDCMP, IDCMP_CLOSEWINDOW }, { WA_BitMap, (uint64_t)b[2] }) == 0);
    picture(&ws.w_RPort, 320, 200, 0x8a5a44, "drag me too");
    CHECK(IIntuition->PresentWindow(&ws, nullptr, 0) == 0);
    stripes(&wm.w_RPort, 640, 480, 0);
    CHECK(IIntuition->PresentWindow(&wm, nullptr, 0) == 0);
    CHECK(on_screen(&wm, 1) && on_screen(&ws, 1));
    mm.x = wm.w_Sync->is_LeftEdge, mm.y = wm.w_Sync->is_TopEdge;
    ms.x = ws.w_Sync->is_LeftEdge, ms.y = ws.w_Sync->is_TopEdge;
    KPrintF("window: drag: drag the windows by their title bars with the mouse. \"drag me\"'s stripes");
    KPrintF("window: drag: move each frame, so a tear shows as a broken stripe; the pointer should be");
    KPrintF("window: drag: smooth. A close gadget ends it, or %lu s.", (uint64_t)DRAG_S);
    CHECK(info(in.in_Call) == 0);
    moves0 = in.in_Call->im_Moves;
    nanos0 = in.in_Call->im_MoveNanos;
    end = CurrentTime() + DRAG_S * 1000 * MS;
    say = CurrentTime() + 30000 * MS;   /* the serial line's watcher gives up after 120 s quiet */
    last = wm.w_Sync->is_DispTime;
    v0 = last_vblank(&t0);
    while (!closed && CurrentTime() < end && frames < sizeof gap / sizeof gap[0]) {
        CHECK(IIntuition->WaitSafe(&wm, CurrentTime() + 1000 * MS) == 0);
        stripes(&wm.w_RPort, 640, 480, frames + 1);
        CHECK(IIntuition->PresentWindow(&wm, nullptr, 0) == 0);
        CHECK(on_screen(&wm, wm.w_Frame));
        gap[frames++] = wm.w_Sync->is_DispTime - last;
        last = wm.w_Sync->is_DispTime;
        sample(&mm, &wm);
        sample(&ms, &ws);
        while (IIntuition->GetIntuiEvent(&in, &ev))
            closed |= ev.iev_Class == IDCMP_CLOSEWINDOW;
        if (CurrentTime() > say) {
            KPrintF("window: drag: %lu frames so far; the windows moved %lu and %lu pixels", frames, mm.dist,
                    ms.dist);
            say += 30000 * MS;
        }
    }
    v1 = last_vblank(&t1);
    CHECK(v1 > v0);
    period = (t1 - t0) / (v1 - v0);
    for (uint64_t f = 0; f < frames; f++) {
        uint64_t k = (gap[f] + period / 2) / period;

        missed += k > 1 ? k - 1 : 0;
    }
    CHECK(IIntuition->CloseWindow(&wm) == 0 && IIntuition->CloseWindow(&ws) == 0);
    for (unsigned i = 0; i < 3; i++)
        IGfx->FreeBitMap(b[i]);
    KPrintF("window: drag: %s; \"drag me\" %lu frames over %lu vblanks, %lu missed",
            closed ? "closed" : "the time's up", frames, v1 - v0, missed);
    KPrintF("window: drag: \"drag me\" moved in %lu frames, %lu pixels, still %lu times for one frame",
            mm.frames, mm.dist, mm.stalls);
    KPrintF("window: drag: \"close me to end\" moved in %lu frames, %lu pixels, still %lu times for one frame",
            ms.frames, ms.dist, ms.stalls);
    CHECK(info(in.in_Call) == 0);
    moves0 = in.in_Call->im_Moves - moves0;
    KPrintF("window: drag: the pointer moved in %lu frames, input's event to the latch %lu us on average,",
            moves0, moves0 ? (in.in_Call->im_MoveNanos - nanos0) / moves0 / 1000 : 0);
    KPrintF("window: drag: %lu us at most (since intuition started); read from the ring %lu us at most",
            in.in_Call->im_MoveMax / 1000, in.in_Call->im_ReadMax / 1000);
    KPrintF("window: drag: moves from input, after the last by <1 ms %lu, <20 ms %lu, <100 ms %lu, <1 s %lu",
            in.in_Call->im_MoveGaps[0], in.in_Call->im_MoveGaps[1], in.in_Call->im_MoveGaps[2],
            in.in_Call->im_MoveGaps[3]);
    return 0;
}

/* --- 5: the mouse and keys. --- */

static void nap(uint64_t ms)
{
    static uint64_t never;              /* a word nobody wakes */

    WaitAddr(&never, 0, CurrentTime() + ms * MS);
}

/* intuition's state now: the pointer kept in ptr_x, ptr_y. */
static int64_t info(struct IntuiMsg *m)
{
    int64_t e;

    memset(m, 0, sizeof *m);
    if ((e = ask(&in, m, INTUI_INFO)) == 0) {
        ptr_x = m->im_PointerX;
        ptr_y = m->im_PointerY;
        ptr_scale = pointer_scale(m->im_Width, m->im_Height);
    }
    return e;
}

/* The pointer to (x, y), asked for 100 pixels at a time (QEMU's mouse
 * reports one at once, 127 at most): false if it didn't move within 2 s
 * of asking (nobody's there to move it). The asks are paced to the
 * kernel's log rate (20 lines a second after 32), so none is dropped. */
static bool point_at(int32_t x, int32_t y)
{
    static uint64_t asked;
    struct IntuiMsg *m = in.in_Call;

    for (unsigned tries = 0; tries < 40; tries++) {
        int32_t was_x = ptr_x, was_y = ptr_y, dx, dy;
        uint64_t until = CurrentTime() + 2000 * MS;

        if (info(m) != 0)
            return false;
        if (ptr_x == x && ptr_y == y)
            return true;
        dx = x - ptr_x;
        dy = y - ptr_y;
        dx = dx > 100 ? 100 : dx < -100 ? -100 : dx;
        dy = dy > 100 ? 100 : dy < -100 ? -100 : dy;
        if (CurrentTime() - asked < 50 * MS)
            nap((asked + 50 * MS - CurrentTime()) / MS + 1);
        asked = CurrentTime();
        KPrintF("window: send move %ld %ld", (int64_t)dx, (int64_t)dy);
        was_x = ptr_x;
        was_y = ptr_y;
        while (info(m) == 0 && ptr_x == was_x && ptr_y == was_y)
            if (CurrentTime() > until)
                return false;
            else
                nap(5);
    }
    return false;
}

static void click(void)
{
    KPrintF("window: send mouse 0 0 left");
}

/* The next event that isn't a move, or a move if 'moves', within 'ms'. */
static bool next_event_within(struct IntuiEvent *ev, bool moves, uint64_t ms)
{
    uint64_t until = CurrentTime() + ms * MS;

    for (;;) {
        while (IIntuition->GetIntuiEvent(&in, ev))
            if (moves || ev->iev_Class != IDCMP_MOUSEMOVE)
                return true;
        if (IIntuition->WaitIntuiEvent(&in, until) != 0)
            return false;
    }
}

static bool next_event(struct IntuiEvent *ev, bool moves)
{
    return next_event_within(ev, moves, 2000);
}

static bool heard(const struct Window *w, uint32_t cls, uint16_t code)
{
    struct IntuiEvent ev;

    if (!next_event(&ev, cls == IDCMP_MOUSEMOVE))
        return false;
    if (ev.iev_Window == w->w_Id && ev.iev_Class == cls && ev.iev_Code == code)
        return true;
    KPrintF("window: heard class %lx code %lx of window %lu, not class %lx code %lx of %lu",
            (uint64_t)ev.iev_Class, (uint64_t)ev.iev_Code, (uint64_t)ev.iev_Window, (uint64_t)cls,
            (uint64_t)code, (uint64_t)w->w_Id);
    return false;
}

/* Clicked at (x, y), and heard as a press and a release there. */
static bool clicked_in(const struct Window *w, int32_t x, int32_t y)
{
    struct IntuiEvent ev;

    if (!point_at(x, y))
        return false;
    click();
    for (unsigned i = 0; i < 2; i++) {
        uint32_t code = i ? SELECTUP : SELECTDOWN;

        do
            if (!next_event(&ev, false))
                return false;
        while (ev.iev_Class == IDCMP_ACTIVEWINDOW || ev.iev_Class == IDCMP_INACTIVEWINDOW);
        if (ev.iev_Window != w->w_Id || ev.iev_Class != IDCMP_MOUSEBUTTONS || ev.iev_Code != code ||
            ev.iev_MouseX != x - w->w_Sync->is_LeftEdge - w->w_BorderLeft ||
            ev.iev_MouseY != y - w->w_Sync->is_TopEdge - w->w_BorderTop)
            return false;
    }
    return true;
}

/* The window intuition says is active, within a second. */
static bool active_is(const struct Window *w)
{
    struct IntuiMsg *m = in.in_Call;
    uint64_t until = CurrentTime() + 1000 * MS;

    while (info(m) == 0 && m->im_Active != (w ? w->w_Id : 0))
        if (CurrentTime() > until)
            return false;
        else
            nap(5);
    return true;
}

/* Keys typed, heard by w alone as these characters; and a key let go. */
static bool typed(const struct Window *w, const char *keys, const char *chars)
{
    struct IntuiEvent ev;
    bool up = false;

    KPrintF("window: send keys %s", keys);
    for (const char *c = chars; *c;) {
        if (!next_event(&ev, false) || ev.iev_Window != w->w_Id)
            return false;
        if (ev.iev_Class == IDCMP_RAWKEY && ev.iev_Code & RAWKEY_UP)
            up = true;
        else if (ev.iev_Class == IDCMP_VANILLAKEY && ev.iev_Code != (uint16_t)*c++)
            return false;
        else if (ev.iev_Class != IDCMP_RAWKEY && ev.iev_Class != IDCMP_VANILLAKEY)
            return false;
    }
    /* The keys going up, and nothing else, until they stop. */
    while (next_event_within(&ev, false, up ? 300 : 2000))
        if (ev.iev_Window != w->w_Id || ev.iev_Class != IDCMP_RAWKEY)
            return false;
        else
            up |= (ev.iev_Code & RAWKEY_UP) != 0;
    return up;
}

/* Dragged by (dx, dy) from (x, y): there within a second. */
static bool dragged(const void *k, int32_t x, int32_t y, int32_t dx, int32_t dy)
{
    struct other *o = (struct other *)other_of(k);
    const struct Window *w = o ? nullptr : k;
    int32_t left = place_of(k).left, top = place_of(k).top;
    struct IntuiMsg *m = in.in_Call;
    uint64_t until;

    if (!point_at(x, y))
        return false;
    KPrintF("window: send drag %ld %ld left", (int64_t)dx, (int64_t)dy);
    until = CurrentTime() + 1000 * MS;
    while (info(m) == 0 && (ptr_x != x + dx || ptr_y != y + dy))
        if (CurrentTime() > until)
            return false;
        else
            nap(5);
    if (w) {
        while (w->w_Sync->is_LeftEdge != left + dx || w->w_Sync->is_TopEdge != top + dy)
            if (CurrentTime() > until)
                return false;
            else
                nap(5);
    } else {
        o->x += dx;
        o->y += dy;
    }
    to_front(k);
    return true;
}

/* A gadget clicked: 'x' from the right of the window's chrome, 'y' down. */
static bool gadget_click(const void *k, int32_t from_right)
{
    struct place p = place_of(k);
    int32_t width = p.iw + 2 * (int32_t)INTUI_BORDER;

    if (!point_at(p.left + width - from_right, p.top + 12))
        return false;
    click();
    return true;
}

constexpr int32_t CLOSE_GADGET = 15, DEPTH_GADGET = 39;    /* their middles, from the right */

static bool requests_are(uint32_t n, uint64_t ms);
static bool click_on(struct GfxRect r);
static int64_t sharers(uint64_t address);

static struct BitMap *cbm, *dbm;
static struct Window wc, wd;
static int64_t hung = -1;

static void ends(void)
{
    if (hung >= 0) {
        KillProcess((Handle)hung);
        CloseHandle((Handle)hung);
    }
    hung = -1;
}

static int input_half(bool readable)
{
    struct IntuiMsg *m = in.in_Call;
    const uint32_t wanted = IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_ACTIVEWINDOW |
                            IDCMP_INACTIVEWINDOW | IDCMP_CLOSEWINDOW;
    const uint32_t chrome = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET;
    struct IntuiEvent ev;
    unsigned windows;

    CHECK(info(m) == 0);
    if (!m->im_Input) {
        KPrintF("window: input: not exercised: intuition holds no input ring");
        return NO_INPUT;
    }
    CHECK((cbm = IGfx->AllocBitMap(200, 120, PIXFMT_XRGB8888, 0, nullptr)) &&
          (dbm = IGfx->AllocBitMap(160, 100, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &wc, { WA_Left, 80 }, { WA_Top, 60 }, { WA_Title, (uint64_t)"wc" },
                         { WA_Flags, chrome }, { WA_IDCMP, wanted | IDCMP_MOUSEMOVE },
                         { WA_BitMap, (uint64_t)cbm }) == 0);
    CHECK(OpenWindowTags(IIntuition, &in, &wd, { WA_Left, 240 }, { WA_Top, 120 }, { WA_Title, (uint64_t)"wd" },
                         { WA_Flags, chrome | WFLG_ACTIVATE }, { WA_IDCMP, wanted },
                         { WA_BitMap, (uint64_t)dbm }) == 0);
    CHECK(wc.w_BorderLeft == (int32_t)INTUI_BORDER && wc.w_BorderTop == (int32_t)INTUI_TITLEBAR);
    picture(&wc.w_RPort, 200, 120, 0x5b88c2, "window c");
    picture(&wd.w_RPort, 160, 100, 0x8a5a44, "window d");
    CHECK(IIntuition->PresentWindow(&wc, nullptr, 0) == 0 && IIntuition->PresentWindow(&wd, nullptr, 0) == 0);
    to_front(&wc);
    to_front(&wd);
    CHECK(on_screen(&wc, 1) && on_screen(&wd, 1));
    CHECK(heard(&wd, IDCMP_ACTIVEWINDOW, 0) && active_is(&wd));
    CHECK(screen_is(readable, 244, 146, 80, 60));       /* the insides' overlap: d's */

    /* Focus moved by a click on c's inside, which comes to the front. */
    if (!point_at(150, 120)) {
        KPrintF("window: input: not exercised: nobody moved the pointer");
        return NO_INPUT;
    }
    click();
    CHECK(heard(&wd, IDCMP_INACTIVEWINDOW, 0) && heard(&wc, IDCMP_ACTIVEWINDOW, 0));
    CHECK(next_event(&ev, false) && ev.iev_Window == wc.w_Id && ev.iev_Class == IDCMP_MOUSEBUTTONS &&
          ev.iev_Code == SELECTDOWN && ev.iev_MouseX == 150 - 84 && ev.iev_MouseY == 120 - 86);
    CHECK(heard(&wc, IDCMP_MOUSEBUTTONS, SELECTUP) && active_is(&wc));
    to_front(&wc);
    CHECK(screen_is(readable, 244, 146, 40, 60));       /* now c's */
    KPrintF("window: send move 5 5");                   /* the active window hears it move */
    do
        CHECK(next_event(&ev, true) && ev.iev_Window == wc.w_Id && ev.iev_Class == IDCMP_MOUSEMOVE);
    while (ev.iev_MouseX != 155 - 84 || ev.iev_MouseY != 125 - 86);

    /* Keys at the active window alone, with the pointer over the other;
     * then at the other once clicked. */
    CHECK(point_at(380, 230));
    CHECK(typed(&wc, "h shift-i", "hI"));
    CHECK(clicked_in(&wd, 380, 230));
    to_front(&wd);
    CHECK(active_is(&wd));
    CHECK(point_at(150, 120));
    CHECK(typed(&wd, "x", "x"));

    /* c dragged by its title bar, from where it was to where it is. */
    CHECK(dragged(&wc, 120, 70, 40, 30));
    CHECK(heard(&wd, IDCMP_INACTIVEWINDOW, 0) && heard(&wc, IDCMP_ACTIVEWINDOW, 0));
    CHECK(screen_is(readable, 124, 116, 200, 120));     /* c's inside, where it went */
    CHECK(screen_is(readable, 80, 60, 40, 140));        /* the backdrop, where it was */

    /* c's depth gadget: to the back, then, from the back, to the front. */
    CHECK(gadget_click(&wc, DEPTH_GADGET));
    to_back(&wc);
    CHECK(screen_is(readable, 250, 150, 60, 60));
    CHECK(gadget_click(&wc, DEPTH_GADGET));
    to_front(&wc);
    CHECK(screen_is(readable, 250, 150, 60, 60));

    /* c's close gadget, and c closed when asked. */
    CHECK(gadget_click(&wc, CLOSE_GADGET));
    CHECK(heard(&wc, IDCMP_CLOSEWINDOW, 0));
    CHECK(IIntuition->CloseWindow(&wc) == 0);
    taken_off(&wc);
    CHECK(screen_is(readable, 250, 150, 60, 60));
    KPrintF("window: input: focus by clicks, keys at the active window alone, a drag, the depth and "
            "close gadgets: each heard as it should be, and the screen read back");

    /* 6: a client that has stopped. */
    CHECK(info(m) == 0);
    windows = m->im_Windows;
    CHECK((hung = CreateProcess("window", STOPPED, SIG_HUNG, 0)) >= 0);
    for (uint64_t until = CurrentTime() + 3000 * MS; info(m) == 0 && m->im_Windows == windows;)
        if (CurrentTime() > until)
            return failed("the stopped client's window never came");
        else
            nap(10);
    nap(50);                            /* its frame on the screen */
    to_front(HUNG);
    CHECK(screen_is(readable, HUNG_X + 4, HUNG_Y + 26, HUNG_W, HUNG_H));
    CHECK(dragged(HUNG, 440, 90, -60, 40));             /* c's close left none active */
    CHECK(screen_is(readable, HUNG->x + 4, HUNG->y + 26, HUNG_W, HUNG_H));
    CHECK(clicked_in(&wd, 300, 240));                   /* d to the front, over it */
    to_front(&wd);
    CHECK(screen_is(readable, 364, 146, 40, 80));       /* where their insides meet */
    CHECK(gadget_click(HUNG, DEPTH_GADGET));            /* it, from the back to the front */
    to_front(HUNG);
    CHECK(screen_is(readable, 364, 146, 40, 80));
    CHECK(gadget_click(HUNG, CLOSE_GADGET));            /* never answered: asked about */
    CHECK(requests_are(1, 3000) && info(m) == 0);
    CHECK(click_on(m->im_ReqButton[0]));                /* Close: hwdb ends it */
    for (uint64_t until = CurrentTime() + 3000 * MS; info(m) == 0 && m->im_Windows != windows;)
        if (CurrentTime() > until)
            return failed("the stopped client's window never went");
        else
            nap(10);
    CHECK(requests_are(0, 1000));
    for (uint64_t until = CurrentTime() + 1000 * MS;; nap(10)) {
        struct ProcessInfo pi;

        if (QueryProcess((Handle)hung, &pi) == 0 && pi.pi_State >= PROCS_EXITED)
            break;
        if (CurrentTime() > until)
            return failed("the stopped client wasn't ended");
    }
    taken_off(HUNG);
    CHECK(screen_is(readable, 408, 146, 76, 80));       /* the backdrop, right of d */
    ends();
    CHECK(IIntuition->CloseWindow(&wd) == 0);
    taken_off(&wd);
    IGfx->FreeBitMap(cbm);
    IGfx->FreeBitMap(dbm);
    KPrintF("window: input: a stopped client's window dragged and raised, asked about a second "
            "after its close gadget, and ended by hwdb on Close");
    return 0;
}

/* --- 6a: menus, a resize and requesters (V8 step 4). --- */

/* Requesters on the screen, within 'ms'. */
static bool requests_are(uint32_t n, uint64_t ms)
{
    struct IntuiMsg *m = in.in_Call;
    uint64_t until = CurrentTime() + ms * MS;

    while (info(m) == 0 && m->im_Requests != n)
        if (CurrentTime() > until)
            return false;
        else
            nap(5);
    return true;
}

/* The menu intuition says is open (0 none, 1 the system's, 2 on the
 * strip's), within a second. */
static bool menu_is(uint32_t menu)
{
    struct IntuiMsg *m = in.in_Call;
    uint64_t until = CurrentTime() + 1000 * MS;

    while (info(m) == 0 && m->im_Menu != menu)
        if (CurrentTime() > until) {
            KPrintF("window: menu %lu open, not %lu", (uint64_t)m->im_Menu, (uint64_t)menu);
            return false;
        } else {
            nap(5);
        }
    return true;
}

static bool click_on(struct GfxRect r)
{
    if (!point_at((r.MinX + r.MaxX) / 2, (r.MinY + r.MaxY) / 2))
        return false;
    click();
    return true;
}

/* The next event but buttons and keys: w's MENUPICK of 'code', a CHECKIT
 * item 'checked' or not. */
static bool picked(const struct Window *w, uint16_t code, uint32_t checked)
{
    struct IntuiEvent ev;

    do
        if (!next_event(&ev, false))
            return false;
    while (ev.iev_Class == IDCMP_MOUSEBUTTONS || ev.iev_Class == IDCMP_RAWKEY ||
           ev.iev_Class == IDCMP_ACTIVEWINDOW || ev.iev_Class == IDCMP_INACTIVEWINDOW);
    if (ev.iev_Window == w->w_Id && ev.iev_Class == IDCMP_MENUPICK && ev.iev_Code == code &&
        ev.iev_Char == checked)
        return true;
    KPrintF("window: heard class %lx code %lx (%lu) of window %lu, not menu pick %lx (%lu)",
            (uint64_t)ev.iev_Class, (uint64_t)ev.iev_Code, (uint64_t)ev.iev_Char, (uint64_t)ev.iev_Window,
            (uint64_t)code, (uint64_t)checked);
    return false;
}

/* w's IDCMP_NEWSIZE, at an inside of iw by ih, and the sync words so. */
static bool sized(const struct Window *w, int32_t iw, int32_t ih)
{
    struct IntuiEvent ev;

    do
        if (!next_event(&ev, false))
            return false;
    while (ev.iev_Class == IDCMP_MOUSEBUTTONS || ev.iev_Class == IDCMP_ACTIVEWINDOW ||
           ev.iev_Class == IDCMP_INACTIVEWINDOW);
    if (ev.iev_Window == w->w_Id && ev.iev_Class == IDCMP_NEWSIZE && ev.iev_MouseX == iw &&
        ev.iev_MouseY == ih && (int32_t)w->w_Sync->is_Width == iw && (int32_t)w->w_Sync->is_Height == ih)
        return true;
    KPrintF("window: heard class %lx at %ld, %ld of window %lu, not a new size of %ld by %ld",
            (uint64_t)ev.iev_Class, (int64_t)ev.iev_MouseX, (int64_t)ev.iev_MouseY, (uint64_t)ev.iev_Window,
            (int64_t)iw, (int64_t)ih);
    return false;
}

static const struct NewMenu strip[] = {
    { NM_TITLE, "Project", nullptr, 0 },
    { NM_ITEM, "Open", nullptr, 0 },
    { NM_ITEM, "Save", "s", 0 },
    { NM_ITEM, NM_BARLABEL, nullptr, 0 },
    { NM_ITEM, "Quit", "Q", 0 },
    { NM_TITLE, "Options", nullptr, 0 },
    { NM_ITEM, "Snap to grid", nullptr, CHECKIT },
    { NM_END, nullptr, nullptr, 0 },
};

/* The EasyRequest of a second task, on a connection of its own, as
 * this one's calls go on meanwhile. */
static struct {
    struct Intuition in;
    int64_t answer;
    uint64_t done;
} easy;

static void asker(uint64_t)
{
    const struct EasyStruct es = { sizeof es, 0, "A question", "Which one?\nThe middle, please.",
                                   "Left|Middle|Right" };

    easy.answer = IIntuition->OpenIntuition(&easy.in, CurrentTime() + 1000 * MS);
    if (easy.answer == 0) {
        easy.answer = IIntuition->EasyRequest(&easy.in, nullptr, &es);
        IIntuition->CloseIntuition(&easy.in);
    }
    __atomic_store_n(&easy.done, 1, __ATOMIC_RELEASE);
    WakeAddr(&easy.done, 1);
    ExitTask(0);
}

static int desktop_half(bool readable)
{
    struct IntuiMsg *m = in.in_Call;
    struct BitMap *ebm, *fbm;
    struct Window we;
    struct GfxRect menu;
    struct place p;
    struct IntuiEvent ev;
    uint32_t want, got;
    int32_t item0, item1;
    uint64_t until;
    int64_t task;

    while (IIntuition->GetIntuiEvent(&in, &ev))
        ;                               /* what the windows before said, closed now */
    CHECK((ebm = IGfx->AllocBitMap(200, 120, PIXFMT_XRGB8888, 0, nullptr)) &&
          (fbm = IGfx->AllocBitMap(240, 150, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &we, { WA_Left, 120 }, { WA_Top, 100 }, { WA_Title, (uint64_t)"menus" },
                         { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SIZEGADGET | WFLG_ACTIVATE },
                         { WA_IDCMP, IDCMP_MENUPICK | IDCMP_NEWSIZE | IDCMP_MOUSEBUTTONS }, { WA_MinWidth, 80 },
                         { WA_MinHeight, 50 }, { WA_BitMap, (uint64_t)ebm }) == 0);
    CHECK(we.w_BorderBottom == (int32_t)INTUI_SIZEBORDER && we.w_BorderRight == (int32_t)INTUI_BORDER);
    picture(&we.w_RPort, 200, 120, 0x46556e, "menus");
    CHECK(IIntuition->SetMenuStrip(&we, strip) == 0);
    CHECK(IIntuition->PresentWindow(&we, nullptr, 0) == 0);
    to_front(&we);
    CHECK(on_screen(&we, 1) && active_is(&we));
    CHECK(screen_is(readable, 124, 126, 200, 120));

    /* A title clicked: its menu over the window, then Save chosen. */
    CHECK(info(m) == 0 && m->im_TitleX[1] > m->im_TitleX[0] && m->im_TitleX[2] > m->im_TitleX[1]);
    CHECK(point_at(m->im_TitleX[1], 14));
    click();
    CHECK(menu_is(2) && info(m) == 0 && m->im_MenuRect.MinY == (int32_t)INTUI_SCREENBAR);
    menu = m->im_MenuRect;
    item0 = m->im_ItemY[0];
    item1 = m->im_ItemY[1];
    if (menu.MaxY > 99)
        menu.MaxY = 99;                 /* above the window's chrome, which the model doesn't know */
    CHECK(expect(menu.MinX, menu.MinY, (uint32_t)(menu.MaxX - menu.MinX + 1), (uint32_t)(menu.MaxY - menu.MinY + 1),
                 &want));
    for (until = CurrentTime() + 1000 * MS; readable;)     /* the menu drawn there, not what's under it */
        if (read_screen(menu.MinX, menu.MinY, (uint32_t)(menu.MaxX - menu.MinX + 1),
                        (uint32_t)(menu.MaxY - menu.MinY + 1), &got) == 0 && got != want)
            break;
        else if (CurrentTime() > until)
            return failed("the menu wasn't drawn over the window");
    CHECK(point_at(menu.MinX + 30, item1));
    click();
    CHECK(picked(&we, FULLMENUNUM(0, 1, NOSUB), 0) && menu_is(0));
    CHECK(screen_is(readable, menu.MinX, menu.MinY, (uint32_t)(menu.MaxX - menu.MinX + 1),
                    (uint32_t)(menu.MaxY - menu.MinY + 1)));

    /* Held by the right button from Options' title to its first item:
     * checked; and Save by its key, with the GUI key. */
    CHECK(info(m) == 0 && point_at(m->im_TitleX[2], 14));
    KPrintF("window: send drag 0 %ld right", (int64_t)(item0 - 14));
    CHECK(picked(&we, FULLMENUNUM(1, 0, NOSUB), 1) && menu_is(0));
    KPrintF("window: send keys meta_l-s");
    CHECK(picked(&we, FULLMENUNUM(0, 1, NOSUB), 0));

    /* The system menu's About, and its requester's OK. */
    CHECK(info(m) == 0 && point_at(m->im_TitleX[0], 14));
    click();
    CHECK(menu_is(1) && info(m) == 0);
    CHECK(point_at(m->im_MenuRect.MinX + 30, m->im_ItemY[0]));
    nap(300);                                           /* its frame shown, then on QEMU */
    KPrintF("window: send screendump menu");            /* saved, to be looked at */
    click();
    CHECK(requests_are(1, 1000) && info(m) == 0);
    nap(300);
    KPrintF("window: send screendump about");
    CHECK(click_on(m->im_ReqButton[0]) && requests_are(0, 1000));
    CHECK(screen_is(readable, 124, 126, 200, 120));
    KPrintF("window: menus: a title clicked open over the window and an item chosen, another held "
            "open by the right button, a key with GUI, the system menu's About and its OK");

    /* The clock, where there's an RTC: intuition looks every 5 s until it's there. */
    {
        struct MsgPort rtc;

        if (ConnectPort(&rtc, "rtc.0", SIG_DISPLAY, 2, 256) == 0) {
            ClosePort(&rtc);
            for (until = CurrentTime() + 6000 * MS; info(m) == 0 && !m->im_Clock;)
                if (CurrentTime() > until)
                    return failed("the screen bar's clock never showed the RTC's time");
                else
                    nap(20);
            KPrintF("window: the clock shows %lu:%02lu UTC, from the RTC", m->im_Clock / 3600 % 24,
                    m->im_Clock / 60 % 60);
        } else {
            KPrintF("window: no RTC here: the screen bar shows no clock");
        }
    }

    /* Resized by its corner, 40 by 30 bigger: its old buffer shown, the
     * backdrop past it. */
    p = place_of(&we);
    CHECK(point_at(p.left + p.bl + p.iw + p.br - 9, p.top + p.bt + p.ih + p.bb - 9));
    KPrintF("window: send drag 40 30 left");
    CHECK(sized(&we, 240, 150) && info(m) == 0);       /* and where the pointer went */
    CHECK(screen_is(readable, p.left + p.bl, p.top + p.bt, 240, 150));
    CHECK(IIntuition->ChangeWindowBuffers(&we, fbm, nullptr) == 0 && we.w_Width == 240 && we.w_Height == 150);
    picture(&we.w_RPort, 240, 150, 0x3a6298, "resized");
    CHECK(IIntuition->PresentWindow(&we, nullptr, 0) == 0 && on_screen(&we, we.w_Frame));
    CHECK(screen_is(readable, p.left + p.bl, p.top + p.bt, 240, 150));
    for (until = CurrentTime() + 1000 * MS; sharers((uint64_t)ebm->bm_BaseAddr) != 1;)
        if (CurrentTime() > until)
            return failed("intuition didn't let go of a resized window's old buffer");
        else
            nap(5);

    /* Far smaller: held to its least size. */
    p = place_of(&we);
    CHECK(point_at(p.left + p.bl + p.iw + p.br - 9, p.top + p.bt + p.ih + p.bb - 9));
    KPrintF("window: send drag -250 -200 left");
    CHECK(sized(&we, 80, 50) && info(m) == 0);
    CHECK(screen_is(readable, p.left + p.bl, p.top + p.bt, 80, 50));
    CHECK(screen_is(readable, p.left + 90, p.top, 240 + 8 - 90, 150 + 26 + 16));    /* where it was */
    KPrintF("window: resize: told the new size, the old buffer shown and the backdrop past it, new "
            "buffers taken and the old let go of; held to its least size");

    /* EasyRequest: a second task asks, this one answers the middle. */
    CHECK((task = CreateTask(asker, 0)) >= 0);
    CHECK(requests_are(1, 3000) && info(m) == 0 && click_on(m->im_ReqButton[1]));
    for (until = CurrentTime() + 2000 * MS; !__atomic_load_n(&easy.done, __ATOMIC_ACQUIRE);)
        if (WaitAddr(&easy.done, 0, until) == ERR_TIMEOUT)
            return failed("EasyRequest wasn't answered");
    CHECK(easy.answer == 2 && requests_are(0, 1000));
    CloseHandle((Handle)task);
    KPrintF("window: EasyRequest: three buttons, the middle clicked, answered 2");

    CHECK(IIntuition->CloseWindow(&we) == 0);
    taken_off(&we);
    IGfx->FreeBitMap(ebm);
    IGfx->FreeBitMap(fbm);
    return 0;
}

/* --- 6b: the Shell in a window (V8 step 5). --- */

/* Every client's windows on the screen, within 'ms'. */
static bool windows_are(uint32_t n, uint64_t ms)
{
    struct IntuiMsg *m = in.in_Call;
    uint64_t until = CurrentTime() + ms * MS;

    while (info(m) == 0 && m->im_Windows != n)
        if (CurrentTime() > until) {
            KPrintF("window: %lu windows on the screen, not %lu", (uint64_t)m->im_Windows, (uint64_t)n);
            return false;
        } else {
            nap(5);
        }
    return true;
}

/* A window's inside, w by h at (x, y), as the host draws it ('crc'),
 * within 'ms'; or the screen can't be read here. */
static bool drawn_as(bool readable, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t crc, uint64_t ms,
                     const char *what)
{
    uint64_t until = CurrentTime() + ms * MS;
    uint32_t got = 0;

    if (!readable)
        return true;
    do
        if (read_screen(x, y, w, h, &got) == 0 && got == crc)
            return true;
        else
            nap(20);
    while (CurrentTime() < until);
    KPrintF("window: %s: %08lx, not the host's %08lx", what, (uint64_t)got, (uint64_t)crc);
    return false;
}

/* con.1's window's inside. */
static bool inside_is(bool readable, uint32_t crc, uint64_t ms, const char *what)
{
    return drawn_as(readable, CONWIN_LEFT + (int32_t)INTUI_BORDER, CONWIN_TOP + (int32_t)INTUI_TITLEBAR, SW_WIDTH,
                    SW_HEIGHT, crc, ms, what);
}

static int shell_half(bool readable)
{
    struct IntuiMsg *m = in.in_Call;
    uint32_t outer_w = SW_WIDTH + 2 * INTUI_BORDER, outer_h = SW_HEIGHT + INTUI_TITLEBAR + INTUI_SIZEBORDER;
    struct MsgPort con;
    int64_t p;

    /* The system menu's Shell: a window, and its Shell's prompt. */
    CHECK(windows_are(0, 1000) && info(m) == 0 && point_at(m->im_TitleX[0], 14));
    click();
    CHECK(menu_is(1) && info(m) == 0);
    CHECK(point_at(m->im_MenuRect.MinX + 30, m->im_ItemY[2]));
    click();
    CHECK(windows_are(1, 5000));
    CHECK(point_at(10, (int32_t)in.in_Height - 40));   /* out of the way of the reading */
    CHECK(inside_is(readable, sw_crc_ready, 5000, "the Shell's window with its prompt"));

    /* A command typed: a script, by a second Shell, on the same console. */
    KPrintF("window: send text %s", sw_typed);
    KPrintF("window: send keys ret");
    CHECK(inside_is(readable, sw_crc_done, 10000, "the Shell's window after the script"));
    KPrintF("window: send screendump shell");
    nap(300);
    KPrintF("window: send text EndShell");
    KPrintF("window: send keys ret");
    CHECK(windows_are(0, 3000));
    CHECK(screen_is(readable, CONWIN_LEFT, CONWIN_TOP, outer_w, outer_h));
    KPrintF("window: Shell in a window: the system menu's, its prompt, a script run by a second Shell on "
            "its console and read back as the host draws it, and EndShell closing it");

    /* A conwin killed under its Shell: the window gone, and the console. */
    CHECK((p = CreateProcess("conwin", 0, SIG_CONWIN, 0)) >= 0);
    CHECK(windows_are(1, 5000) &&
          inside_is(readable, sw_crc_ready, 5000, "the Shell's window with its prompt, again"));
    CHECK(KillProcess((Handle)p) == 0);
    CHECK(windows_are(0, 100));
    CHECK(screen_is(readable, CONWIN_LEFT, CONWIN_TOP, outer_w, outer_h));
    CHECK(ConnectPort(&con, "con.1", SIG_CONWIN, 2, 256) == ERR_NOTFOUND);
    CloseHandle((Handle)p);
    KPrintF("window: Shell in a window: conwin killed under it, the window and con.1 gone at once");
    return 0;
}

/* --- 6c: ui.library's gadgets (V8 step 6). --- */

static struct UIIFace *IUI;

/* The window's events into its UI, each change drawn and presented, until
 * the step's own are in: a click's or a drag's button let go of, the
 * text's characters, a key down. The last change in *last. */
static bool pump(struct Window *w, struct UI *ui, const struct uw_step *s, struct UIChange *last)
{
    struct IntuiEvent ev;
    struct UIChange c;
    uint64_t chars = 0, want = 0;

    while (s->action == UW_TEXT && s->text[want])
        want++;
    *last = (struct UIChange){};
    while (next_event(&ev, true)) {
        bool down = !(ev.iev_Code & RAWKEY_UP);

        if (ev.iev_Window != w->w_Id)
            continue;
        if (IIntuition->WaitSafe(w, CurrentTime() + 1000 * MS) != 0)
            return false;
        if (IUI->HandleUIEvent(ui, &ev, &c))
            *last = c;
        if (ui->ui_NumDamage && IIntuition->PresentWindow(w, ui->ui_Damage, ui->ui_NumDamage) != 0)
            return false;
        ui->ui_NumDamage = 0;
        if ((s->action == UW_CLICK || s->action == UW_DRAG) && ev.iev_Class == IDCMP_MOUSEBUTTONS &&
            ev.iev_Code == SELECTUP)
            return true;
        if (s->action == UW_TEXT && ev.iev_Class == IDCMP_VANILLAKEY && ++chars == want)
            return true;
        if (s->action == UW_KEY && down && (ev.iev_Class == IDCMP_VANILLAKEY || ev.iev_Class == IDCMP_RAWKEY))
            return true;
    }
    return false;
}

static int ui_half(bool readable)
{
    static struct UI ui;
    static char field[UW_FIELD];
    struct Library *lib;
    struct BitMap *bm;
    struct Window w;
    struct UIChange last;
    struct IntuiEvent ev;
    int32_t ox, oy, x, y;

    CHECK((lib = IExec->OpenLibrary("ui.library", 8)) && lib->lib_Version == 8);
    CHECK((IUI = (struct UIIFace *)IExec->GetInterface(lib, "main", 1)));
    CHECK(IUI->Data.if_Size == sizeof(struct UIIFace));
    CHECK(IUI->InitUI(&ui, IGfx) == 0 && uw_build(IUI, &ui, field));
    CHECK((bm = IGfx->AllocBitMap(UW_WIDTH, UW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &w, { WA_Left, UW_LEFT }, { WA_Top, UW_TOP },
                         { WA_Title, (uint64_t)"ui.library" },
                         { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE },
                         { WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY | IDCMP_RAWKEY },
                         { WA_BitMap, (uint64_t)bm }) == 0);
    ui.ui_RPort = &w.w_RPort;
    IUI->LayoutUI(&ui, w.w_Width, w.w_Height);
    IUI->DrawUI(&ui);
    ui.ui_NumDamage = 0;
    CHECK(IIntuition->PresentWindow(&w, nullptr, 0) == 0);
    ox = UW_LEFT + w.w_BorderLeft;
    oy = UW_TOP + w.w_BorderTop;
    CHECK(point_at(10, (int32_t)in.in_Height - 40));
    CHECK(drawn_as(readable, ox, oy, UW_WIDTH, UW_HEIGHT, uw_crc_ready, 3000, "ui.library's window, first drawn"));

    /* Clicked, dragged and typed through, as uiwin.h says, each change as it says. */
    for (unsigned i = 0; i < sizeof uw_script / sizeof uw_script[0]; i++) {
        const struct uw_step *s = &uw_script[i];

        if (s->action == UW_CLICK || s->action == UW_DRAG) {
            uw_point(&ui, s, &x, &y);
            CHECK(point_at(ox + x, oy + y));
        }
        if (s->action == UW_CLICK)
            click();
        else if (s->action == UW_DRAG)
            KPrintF("window: send drag %ld 0 left", (int64_t)s->drag);
        else
            KPrintF("window: send %s %s", s->action == UW_TEXT ? "text" : "keys", s->text);
        CHECK(pump(&w, &ui, s, &last));
        if (last.uc_Gadget != s->want || last.uc_Value != s->value || last.uc_Flags != s->flags)
            KPrintF("window: ui.library's step %lu: gadget %lu changed to %ld (%lx), not %lu to %ld (%lx)",
                    (uint64_t)i + 1, (uint64_t)last.uc_Gadget, (int64_t)last.uc_Value, (uint64_t)last.uc_Flags,
                    (uint64_t)s->want, (int64_t)s->value, (uint64_t)s->flags);
        CHECK(last.uc_Gadget == s->want && last.uc_Value == s->value && last.uc_Flags == s->flags);
    }
    CHECK(point_at(10, (int32_t)in.in_Height - 40));
    while (next_event_within(&ev, true, 300))            /* the keys let go of, the pointer's moves */
        ;
    CHECK(drawn_as(readable, ox, oy, UW_WIDTH, UW_HEIGHT, uw_crc_done, 3000, "ui.library's window, clicked through"));
    KPrintF("window: send screendump ui");
    nap(300);
    CHECK(IIntuition->CloseWindow(&w) == 0);
    IUI->FreeUI(&ui);
    IGfx->FreeBitMap(bm);
    KPrintF("window: ui.library: a window of every gadget, clicked, dragged and typed through, %lu steps, each "
            "change as the host's, read back as the host draws it", (uint64_t)(sizeof uw_script / sizeof uw_script[0]));
    return 0;
}

/* --- 6d: the Workbench and Prefs (V8 step 7). --- */

/* The checksum DISPLAY_READ gives of w by h of a bitmap at (x, y). */
/* DISPLAY_READ's CRC-32, a pixel's three bytes and a zero at a time. */
static uint32_t crc_step(uint32_t crc, uint32_t px)
{
    static uint32_t table[256];

    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;

            for (unsigned k = 0; k < 8; k++)
                c = c >> 1 ^ (0xedb88320u & -(c & 1));
            table[i] = c;
        }
    px &= 0xffffff;
    for (unsigned k = 0; k < 32; k += 8)
        crc = crc >> 8 ^ table[(crc ^ px >> k) & 0xff];
    return crc;
}

static uint32_t crc_rect(const struct BitMap *bm, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    uint32_t crc = ~0u;

    for (uint32_t y = y0; y < y0 + h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow);

        for (uint32_t x = x0; x < x0 + w; x++)
            crc = crc_step(crc, row[x]);
    }
    return ~crc;
}

static struct Dos wdos;

/* input's settings: INPUT_GET or INPUT_SET, on a connection made for it. */
static int64_t input_prefs(uint32_t op, struct InputPrefs *p)
{
    static struct InputMsg m;
    int64_t e;

    m = (struct InputMsg){ .im_Msg.mn_Length = sizeof m, .im_Op = op, .im_Prefs = *p };
    if ((e = CallPort("input", SIG_INPUT, &m.im_Msg, CurrentTime() + 1000 * MS)) == 0)
        *p = m.im_Prefs;
    return e;
}

/* input's settings as 'want' says, within 'ms'. */
static bool input_is(uint32_t delay, uint32_t rate, uint32_t speed, uint64_t ms)
{
    uint64_t until = CurrentTime() + ms * MS;
    struct InputPrefs p = {};

    do
        if (input_prefs(INPUT_GET, &p) == 0 && p.ip_RepeatDelay == delay && p.ip_RepeatRate == rate &&
            p.ip_PointerSpeed == speed)
            return true;
        else
            nap(10);
    while (CurrentTime() < until);
    KPrintF("window: input's settings are delay %lu, rate %lu, speed %lu; not %lu, %lu, %lu",
            (uint64_t)p.ip_RepeatDelay, (uint64_t)p.ip_RepeatRate, (uint64_t)p.ip_PointerSpeed, (uint64_t)delay,
            (uint64_t)rate, (uint64_t)speed);
    return false;
}

static bool hud_is(uint32_t hud, uint64_t ms)
{
    uint64_t until = CurrentTime() + ms * MS;
    struct IntuiPrefs p = {};

    do
        if (IIntuition->GetPrefs(&in, &p) == 0 && p.ip_Hud == hud)
            return true;
        else
            nap(10);
    while (CurrentTime() < until);
    return false;
}

/* A window of the Workbench's, w by h inside, at (x, y) of its chrome,
 * read back as wb.h draws path's entries: how many, or -1. */
static int drawer_is(bool readable, const char *path, int32_t x, int32_t y, struct UI *ui, struct wb_drawer *d)
{
    struct BitMap *bm;
    struct RastPort rp;
    bool ok;

    if (wb_read(&wdos, path, d) != 0 || IUI->InitUI(ui, IGfx) != 0 || !wb_drawer_build(IUI, ui, d) ||
        !(bm = IGfx->AllocBitMap(WB_DRAWER_W, WB_DRAWER_H, PIXFMT_XRGB8888, 0, nullptr)))
        return -1;
    IGfx->InitRastPort(&rp, bm);
    ui->ui_RPort = &rp;
    IUI->LayoutUI(ui, WB_DRAWER_W, WB_DRAWER_H);
    IUI->DrawUI(ui);
    ok = drawn_as(readable, x + (int32_t)INTUI_BORDER, y + (int32_t)INTUI_TITLEBAR, WB_DRAWER_W, WB_DRAWER_H,
                  crc_rect(bm, 0, 0, WB_DRAWER_W, WB_DRAWER_H), 5000, path);
    ui->ui_RPort = nullptr;
    IGfx->FreeBitMap(bm);
    return ok ? (int)d->count : -1;
}

/* The Workbench's backdrop window, in the colours 'top' and 'bottom',
 * with the volumes' icons, as wb.h draws it, within 'ms'. The pointer
 * must be at the screen's top right, on the screen bar but for a column. */
static bool desk_is(bool readable, const struct wb_icon *icons, unsigned n, uint32_t top, uint32_t bottom,
                    uint64_t ms)
{
    uint32_t bw = in.in_Width, bh = in.in_Height - INTUI_SCREENBAR;
    struct TextFont *sans;
    struct BitMap *bm;
    struct RastPort rp;
    bool ok;

    if (!readable)
        return true;
    if (!(sans = IGfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 })))
        return false;
    if (!(bm = IGfx->AllocBitMap(bw, bh, PIXFMT_XRGB8888, 0, nullptr))) {
        IGfx->CloseTextFont(sans);
        return false;
    }
    IGfx->InitRastPort(&rp, bm);
    wb_backdrop(IGfx, &rp, bw, bh, top, bottom);
    for (unsigned i = 0; i < n; i++)
        wb_icon_draw(IGfx, &rp, sans, &icons[i]);
    ok = drawn_as(readable, 0, INTUI_SCREENBAR, bw - 1, bh, crc_rect(bm, 0, 0, bw - 1, bh), ms,
                  "the Workbench's backdrop and volumes");
    IGfx->FreeBitMap(bm);
    IGfx->CloseTextFont(sans);
    return ok;
}

/* A click on Prefs' window, given to this program's copy of it too:
 * the change that gives. */
static bool prefs_click(struct UI *ui, uint32_t gadget, int32_t dx, int32_t dy, struct UIChange *c)
{
    struct IntuiEvent ev = { CurrentTime(), IDCMP_MOUSEBUTTONS, 1, SELECTDOWN, 0, 0, 0, 0 };
    struct UIChange got;

    pw_point(ui, gadget, dx, dy, &ev.iev_MouseX, &ev.iev_MouseY);
    *c = (struct UIChange){};
    if (!point_at(PW_LEFT + (int32_t)INTUI_BORDER + ev.iev_MouseX, PW_TOP + (int32_t)INTUI_TITLEBAR + ev.iev_MouseY))
        return false;
    click();
    if (IUI->HandleUIEvent(ui, &ev, &got))
        *c = got;
    ev.iev_Code = SELECTUP;
    if (IUI->HandleUIEvent(ui, &ev, &got))
        *c = got;
    nap(300);                           /* Prefs' own, set before what's next */
    return c->uc_Gadget == gadget;
}

static int workbench_half(bool readable)
{
    static struct wb_icon icons[WB_VOLUMES];
    static struct wb_drawer d;
    static struct UI ui;
    struct IntuiMsg *m = in.in_Call;
    struct IntuiEvent ev;
    struct InputPrefs ip = {}, was;
    struct IntuiPrefs np = {};
    struct TagItem t[PREFS_TAGS + 1];
    struct UIChange c;
    struct GfxRect lr;
    uint32_t rate, speed;
    int32_t x0, y0, x1, y1, px0;
    int64_t proc;
    unsigned n, k;
    int count;

    while (IIntuition->GetIntuiEvent(&in, &ev))
        ;
    CHECK(OpenDosUntil(&wdos, SIG_DOS, CurrentTime() + 5000 * MS) == 0);
    CHECK((n = wb_volumes(&wdos, icons, WB_VOLUMES)) > 0);
    CHECK(input_prefs(INPUT_GET, &ip) == 0 && IIntuition->GetPrefs(&in, &np) == 0);
    CHECK(ip.ip_PointerSpeed == INPUT_SPEED_DEFAULT && np.ip_Hud == 0);
    was = ip;

    /* The backdrop and the volumes, read back with the pointer at the
     * screen's top right, on the screen bar but for a column. */
    CHECK((proc = CreateProcess("workbench", 0, SIG_WB, 0)) > 0);
    CHECK(windows_are(1, 10000));
    CHECK(point_at((int32_t)in.in_Width - 1, 0));
    CHECK(desk_is(readable, icons, n, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM, 5000));
    KPrintF("window: the Workbench: its backdrop and dos's %lu volumes' icons, read back as wb.h draws them",
            (uint64_t)n);

    /* The first volume opened by a double click: its window lists what dos does. */
    CHECK(point_at(icons[0].x + WB_ICON / 2, INTUI_SCREENBAR + icons[0].y + WB_ICON / 2));
    click();
    click();
    CHECK(windows_are(2, 5000));
    {
        char path[WB_NAME + 1];

        SNPrintf(path, sizeof path, "%s:", icons[0].name);
        wb_drawer_at(0, &x0, &y0);
        CHECK(point_at(x0 - 20, y0 + 20));      /* off its window */
        CHECK((count = drawer_is(readable, path, x0, y0, &ui, &d)) >= 0);
        KPrintF("window: the Workbench: %s opened by a double click, its window listing its %lu entries as dos "
                "lists them", path, (uint64_t)count);

        /* A drawer in it, opened from its list the same way. */
        for (k = 0; k < d.count && !d.dir[k]; k++)
            ;
        CHECK(k < d.count);
        lr = ui.ui_Gadget[WB_D_LIST].g_Rect;
        CHECK(point_at(x0 + (int32_t)INTUI_BORDER + lr.MinX + 40,
                       y0 + (int32_t)INTUI_TITLEBAR + lr.MinY + 2 + 1 + (int32_t)k * 22 + 11));
        click();
        click();
        CHECK(windows_are(3, 5000));
        IUI->FreeUI(&ui);
        SNPrintf(path, sizeof path, "%s:%s", icons[0].name, d.label[k]);
        path[wb_len(path) - 1] = 0;     /* the label's '/' */
        wb_drawer_at(1, &x1, &y1);
        CHECK(point_at(x0 - 20, y0 + 20));
        CHECK((count = drawer_is(readable, path, x1, y1, &ui, &d)) >= 0);
        IUI->FreeUI(&ui);
        KPrintF("window: the Workbench: the drawer %s opened from its list, its %lu entries as dos lists them", path,
                (uint64_t)count);
    }

    /* Tools' Prefs, from the screen bar: the strip's third menu. */
    CHECK(info(m) == 0 && m->im_TitleX[3]);
    CHECK(point_at(m->im_TitleX[3], INTUI_SCREENBAR / 2));
    click();
    CHECK(menu_is(4) && info(m) == 0 && m->im_ItemY[1]);       /* Tools, the bar's fourth title */
    CHECK(point_at((m->im_MenuRect.MinX + m->im_MenuRect.MaxX) / 2, m->im_ItemY[1]));
    click();
    CHECK(windows_are(4, 5000));
    CHECK(IUI->InitUI(&ui, IGfx) == 0 && pw_build(IUI, &ui, &ip, &np, "", ""));
    IUI->LayoutUI(&ui, PW_WIDTH, PW_HEIGHT);

    /* The repeat rate, then the pointer's speed: each a click on its slider. */
    CHECK(prefs_click(&ui, PW_RATE, -30, 11, &c));
    rate = (uint32_t)c.uc_Value;
    CHECK(rate != INPUT_RATE_DEFAULT && input_is(was.ip_RepeatDelay, rate, INPUT_SPEED_DEFAULT, 2000));
    CHECK(prefs_click(&ui, PW_SPEED, -20, 11, &c));
    speed = (uint32_t)c.uc_Value;
    CHECK(speed > 200 && input_is(was.ip_RepeatDelay, rate, speed, 2000));
    CHECK(info(m) == 0);
    px0 = ptr_x;
    KPrintF("window: send move 10 0");
    for (uint64_t until = CurrentTime() + 2000 * MS; info(m) == 0 && ptr_x == px0;)
        CHECK(CurrentTime() < until);
    nap(100);
    CHECK(info(m) == 0);
    if (ptr_x - px0 != (int32_t)(10 * speed / 100))
        KPrintF("window: a move of 10 at %lu%% moved the pointer %ld", (uint64_t)speed, (int64_t)(ptr_x - px0));
    CHECK(ptr_x - px0 == (int32_t)(10 * speed / 100));
    /* Back to as it was, so the pointer goes where it's asked to again;
     * Prefs still has 'speed', to save. */
    ip = (struct InputPrefs){ was.ip_RepeatDelay, rate, INPUT_SPEED_DEFAULT, was.ip_PointerAccel };
    CHECK(input_prefs(INPUT_SET, &ip) == 0);

    /* The HUD on; then Save. */
    CHECK(prefs_click(&ui, PW_HUD, -20, 13, &c) && c.uc_Value == 1 && hud_is(1, 2000));
    CHECK(point_at(x0 - 20, y0 + 20));
    nap(1200);                          /* the HUD's first reading */
    KPrintF("window: send screendump workbench");
    nap(300);
    CHECK(prefs_click(&ui, PW_SAVE, 30, 13, &c));
    CHECK(windows_are(3, 5000));
    IUI->FreeUI(&ui);
    CHECK(ReadPrefs(&wdos, "input", t, PREFS_TAGS + 1) == 4 && t[1].ti_Tag == PREFS_RepeatRate &&
          t[1].ti_Data == rate && t[2].ti_Tag == PREFS_PointerSpeed && t[2].ti_Data == speed);
    CHECK(ReadPrefs(&wdos, "intuition", t, PREFS_TAGS + 1) == 4 && t[0].ti_Tag == PREFS_Hud && t[0].ti_Data == 1);
    KPrintF("window: Prefs: the repeat rate %lu and the pointer's speed %lu%% set by clicks, input agreeing and a "
            "move of 10 moving it %ld; the HUD on, intuition agreeing; saved, and read back",
            (uint64_t)rate, (uint64_t)speed, (int64_t)(10 * speed / 100));

    /* As they were, for what's next; the files stay, for the second round. */
    CHECK(input_prefs(INPUT_SET, &was) == 0);
    np.ip_Hud = 0;
    CHECK(IIntuition->SetPrefs(&in, &np) == 0);
    CloseHandle((Handle)proc);
    CHECK(windows_are(0, 5000));
    CloseDos(&wdos);
    while (IIntuition->GetIntuiEvent(&in, &ev))
        ;
    return 0;
}

/* SURVIVE_MOUSE's: 6d's saved settings in force again after input's and
 * intuition's deaths; then put back, and the files gone. */
static int saved_back(void)
{
    struct TagItem t[PREFS_TAGS + 1];
    struct InputPrefs ip = { INPUT_DELAY_DEFAULT, INPUT_RATE_DEFAULT, INPUT_SPEED_DEFAULT, INPUT_ACCEL_DEFAULT };
    struct IntuiPrefs np = { 0, INTUI_POINTER_AUTO, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM };

    CHECK(OpenDosUntil(&wdos, SIG_DOS, CurrentTime() + 5000 * MS) == 0);
    CHECK(ReadPrefs(&wdos, "input", t, PREFS_TAGS + 1) == 4);
    CHECK(t[2].ti_Data != INPUT_SPEED_DEFAULT);
    CHECK(input_is((uint32_t)t[0].ti_Data, (uint32_t)t[1].ti_Data, (uint32_t)t[2].ti_Data, 5000));
    CHECK(hud_is(1, 1000));
    KPrintF("window: input and intuition killed: each took its saved settings at its start (the pointer's speed "
            "%lu%%, the HUD on)", t[2].ti_Data);
    CHECK(input_prefs(INPUT_SET, &ip) == 0 && IIntuition->SetPrefs(&in, &np) == 0);
    CHECK(DeletePath(&wdos, "SYS:Prefs/Env-Archive/input.prefs") == 0 &&
          DeletePath(&wdos, "SYS:Prefs/Env-Archive/intuition.prefs") == 0);
    CloseDos(&wdos);
    return 0;
}

/* 6, the other end: a window, then nothing ever again. */
static int stopped(void)
{
    struct BitMap *bm;
    struct Window w;

    if (open_lib() || IIntuition->OpenIntuition(&in, CurrentTime() + 1000 * MS) != 0 ||
        !(bm = IGfx->AllocBitMap(HUNG_W, HUNG_H, PIXFMT_XRGB8888, 0, nullptr)) ||
        OpenWindowTags(IIntuition, &in, &w, { WA_Left, HUNG_X }, { WA_Top, HUNG_Y },
                       { WA_Title, (uint64_t)"stopped" },
                       { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET },
                       { WA_IDCMP, IDCMP_CLOSEWINDOW }, { WA_BitMap, (uint64_t)bm }) != 0)
        return 1;
    w.w_RPort.rp_FgPen = 0xff000000 | HUNG_COLOUR;
    IGfx->RectFill(&w.w_RPort, 0, 0, HUNG_W - 1, HUNG_H - 1);
    if (IIntuition->PresentWindow(&w, nullptr, 0) != 0)
        return 2;
    for (;;)
        nap(1000000);
}

/* --- 7: kill-window. --- */

/* The killed client's, to this program: its windows' bitmaps, which it
 * shares with this program as well as intuition, so that this one can
 * see them unshared. */
struct kill_msg {
    struct Message km_Msg;
    uint64_t km_Bitmap[2];
};

static int64_t sharers(uint64_t address)
{
    struct AreaInfo ai;

    return QueryArea((const void *)address, &ai) == 0 ? (int64_t)ai.ai_Sharers : -1;
}

static int kill_window(bool readable)
{
    struct IntuiMsg *m = in.in_Call;
    struct MsgPort kp;
    struct kill_msg *km;
    struct BitMap *ebm;
    struct Window we;
    uint64_t bitmap[2], until, v0, vblanks;
    int64_t port, child;
    unsigned windows;

    CHECK(info(m) == 0);
    windows = m->im_Windows;
    /* One of this program's, under the killed client's two. */
    CHECK((ebm = IGfx->AllocBitMap(160, 100, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &we, { WA_Left, 100 }, { WA_Top, 300 },
                         { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)ebm }) == 0);
    picture(&we.w_RPort, 160, 100, 0x3a6298, "window e");
    CHECK(IIntuition->PresentWindow(&we, nullptr, 0) == 0);
    to_front(&we);
    CHECK(on_screen(&we, 1));

    CHECK((port = CreatePort("window.kill", SIG_KILL)) >= 0);
    CHECK((child = CreateProcess("window", KILLED | (uint64_t)FindTask(nullptr) << 32, SIG_HUNG, 0)) >= 0);
    until = CurrentTime() + 5000 * MS;
    while (AcceptPort(&kp, (Handle)port, SIG_KILL) != 0) {
        CHECK(CurrentTime() < until);
        WaitUntil(1ULL << SIG_KILL | 1ULL << SIG_HUNG, until);
    }
    CHECK((km = (struct kill_msg *)WaitPort(&kp)) && km->km_Msg.mn_Length == sizeof *km);
    bitmap[0] = km->km_Bitmap[0];
    bitmap[1] = km->km_Bitmap[1];
    ReplyMsg(&kp, &km->km_Msg);
    for (until = CurrentTime() + 3000 * MS; info(m) == 0 && m->im_Windows != windows + 3;)
        if (CurrentTime() > until)
            return failed("the killed client's windows never came");
        else
            nap(10);
    to_front(&others[1]);
    to_front(&others[2]);
    CHECK(sharers(bitmap[0]) == 3 && sharers(bitmap[1]) == 3);     /* it, intuition, this */
    CHECK(screen_is(readable, KILL_X + 4, KILL_Y + 26, KILL_W, KILL_H));
    CHECK(screen_is(readable, KILL2_X, KILL2_Y, KILL2_W, KILL2_H));

    /* Killed: gone from the screen, and its bitmaps this program's alone. */
    v0 = dsync->ds_Vblank;
    CHECK(KillProcess((Handle)child) == 0);
    taken_off(&others[1]);
    taken_off(&others[2]);
    CHECK(screen_is(readable, 40, 280, 360, 180));
    vblanks = dsync->ds_Vblank - v0;
    CHECK(!readable || vblanks <= 4);   /* the next latch, but for reading back under TCG */
    for (until = CurrentTime() + 1000 * MS; sharers(bitmap[0]) != 1 || sharers(bitmap[1]) != 1;)
        if (CurrentTime() > until)
            return failed("intuition didn't let go of a killed client's bitmaps");
        else
            nap(5);
    CHECK(info(m) == 0 && m->im_Windows == windows + 1);
    for (unsigned i = 0; i < 2; i++)
        CHECK(ReleaseArea((const void *)bitmap[i]) == 0 && sharers(bitmap[i]) < 0);    /* freed */
    CloseHandle((Handle)child);
    ClosePort(&kp);
    DeletePort((Handle)port);
    CHECK(IIntuition->CloseWindow(&we) == 0);
    taken_off(&we);
    IGfx->FreeBitMap(ebm);
    if (readable)
        KPrintF("window: kill-window: a killed client's two windows gone from the screen as read "
                "back %lu vblanks after, its bitmaps unshared and freed", vblanks);
    else
        KPrintF("window: kill-window: a killed client's two windows gone, its bitmaps unshared and freed");
    return 0;
}

/* 7, the other end: two windows, then killed. It shares their bitmaps
 * with this program's first self once that has answered on its
 * connection, as an Area is shared only with a peer. */
static int killed(Handle parent)
{
    const uint32_t colour[2] = { KILL_COLOUR, KILL2_COLOUR };
    struct BitMap *bm[2];
    struct Window w[2];
    struct MsgPort mp;
    struct kill_msg *km;

    if (open_lib() || IIntuition->OpenIntuition(&in, CurrentTime() + 1000 * MS) != 0 ||
        !(bm[0] = IGfx->AllocBitMap(KILL_W, KILL_H, PIXFMT_XRGB8888, 0, nullptr)) ||
        !(bm[1] = IGfx->AllocBitMap(KILL2_W, KILL2_H, PIXFMT_XRGB8888, 0, nullptr)) ||
        ConnectPortUntil(&mp, "window.kill", SIG_KILL, 2, 2 * sizeof *km, CurrentTime() + 3000 * MS) != 0 ||
        !(km = AllocMsg(&mp, sizeof *km)))
        return 1;
    km->km_Msg.mn_Length = sizeof *km;
    km->km_Bitmap[0] = (uint64_t)bm[0]->bm_BaseAddr;
    km->km_Bitmap[1] = (uint64_t)bm[1]->bm_BaseAddr;
    if (DoMsg(&mp, &km->km_Msg) != 0 || ShareArea(bm[0]->bm_Area, parent, AREAR_READ) != 0 ||
        ShareArea(bm[1]->bm_Area, parent, AREAR_READ) != 0)
        return 2;
    if (OpenWindowTags(IIntuition, &in, &w[0], { WA_Left, KILL_X }, { WA_Top, KILL_Y },
                       { WA_Title, (uint64_t)"killed" },
                       { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET },
                       { WA_BitMap, (uint64_t)bm[0] }) != 0 ||
        OpenWindowTags(IIntuition, &in, &w[1], { WA_Left, KILL2_X }, { WA_Top, KILL2_Y },
                       { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)bm[1] }) != 0)
        return 3;
    for (unsigned i = 0; i < 2; i++) {
        w[i].w_RPort.rp_FgPen = 0xff000000 | colour[i];
        IGfx->RectFill(&w[i].w_RPort, 0, 0, (int32_t)w[i].w_Width - 1, (int32_t)w[i].w_Height - 1);
        if (IIntuition->PresentWindow(&w[i], nullptr, 0) != 0)
            return 4;
    }
    for (;;)
        nap(1000000);
}

/* --- 8: across intuition's, display's and input's deaths. --- */

static struct BitMap *sbm[4];
static struct Window sa, sb, sc;

/* The three windows opened, from the same bitmaps each time: the first
 * two checked on the screen, then the third, hwdb's cue. */
static int open_three(bool readable)
{
    CHECK(info(in.in_Call) == 0);
    CHECK(OpenWindowTags(IIntuition, &in, &sa, { WA_Left, 64 }, { WA_Top, 48 }, { WA_Flags, WFLG_BORDERLESS },
                         { WA_BitMap, (uint64_t)sbm[0] }, { WA_BackBitMap, (uint64_t)sbm[1] }) == 0);
    CHECK(OpenWindowTags(IIntuition, &in, &sb, { WA_Left, 300 }, { WA_Top, 200 }, { WA_Title, (uint64_t)"sb" },
                         { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET },
                         { WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW },
                         { WA_BitMap, (uint64_t)sbm[2] }) == 0);
    CHECK(IIntuition->PresentWindow(&sa, nullptr, 0) == 0 && IIntuition->PresentWindow(&sb, nullptr, 0) == 0);
    to_front(&sa);
    to_front(&sb);
    CHECK(on_screen(&sa, 1) && on_screen(&sb, 1));
    CHECK(screen_is(readable, 64, 48, 200, 120));
    CHECK(screen_is(readable, 304, 226, 160, 100));
    CHECK(OpenWindowTags(IIntuition, &in, &sc, { WA_Left, (int64_t)in.in_Width - 24 }, { WA_Top, 8 },
                         { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)sbm[3] }) == 0);
    CHECK(IIntuition->PresentWindow(&sc, nullptr, 0) == 0);
    to_front(&sc);
    return 0;
}

/* A frame of sa, once it's safe to draw: a second at most. */
static int64_t present_one(void)
{
    int64_t e = IIntuition->WaitSafe(&sa, CurrentTime() + 1000 * MS);

    if (e != 0)
        return e;
    tick(&sa.w_RPort, sa.w_Frame + 1);
    return IIntuition->PresentWindow(&sa, &bar, 1);
}

static int survive(bool mouse)
{
    struct IntuiMsg *m;
    struct IntuiEvent ev;
    struct ProcessInfo p0, p1;
    struct RastPort rp;
    uint32_t want_win, want_bg, got = 0;
    uint64_t until, away = 0;
    int32_t px, py;
    unsigned opens;
    bool readable, input;
    int64_t e;
    int r;

    if ((r = open_lib()))
        return r;
    CHECK(IIntuition->OpenIntuition(&in, CurrentTime() + 5000 * MS) == 0);
    CHECK(display_up(0) == 0);
    readable = read_screen(0, 0, 1, 1, &got) == 0;
    CHECK((sbm[0] = IGfx->AllocBitMap(200, 120, PIXFMT_XRGB8888, 0, nullptr)) &&
          (sbm[1] = IGfx->AllocBitMap(200, 120, PIXFMT_XRGB8888, 0, nullptr)) &&
          (sbm[2] = IGfx->AllocBitMap(160, 100, PIXFMT_XRGB8888, 0, nullptr)) &&
          (sbm[3] = IGfx->AllocBitMap(16, 16, PIXFMT_XRGB8888, 0, nullptr)));
    for (unsigned i = 0; i < 4; i++) {
        IGfx->InitRastPort(&rp, sbm[i]);
        picture(&rp, sbm[i]->bm_Width, sbm[i]->bm_Height, i < 2 ? 0x5b88c2 : 0x8a5a44, "survivor");
    }
    if ((r = open_three(readable)))
        return r;
    CHECK(QueryProcess(0, &p0) == 0);

    /* intuition killed (hwdb's cue was the third window): its page is
     * woken as it goes. */
    until = CurrentTime() + 30000 * MS;
    while ((e = IIntuition->WaitIntuiEvent(&in, until)) == 0)
        while (IIntuition->GetIntuiEvent(&in, &ev))
            ;
    CHECK(e == ERR_PORT_DIED);
    /* The console shows through where its windows were, neither they nor the backdrop. */
    CHECK(expect(64, 48, 200, 120, &want_win));
    nshown = 0;
    ptr_x = ptr_y = -100;
    CHECK(expect(64, 48, 200, 120, &want_bg));
    for (until = CurrentTime() + 1000 * MS; readable;)
        if (read_screen(64, 48, 200, 120, &got) == 0 && got != want_win && got != want_bg)
            break;
        else if (CurrentTime() > until)
            return failed("the console didn't show through once intuition had died");
    IIntuition->CloseIntuition(&in);

    /* Connected to as soon as it's back, and the windows opened again
     * from the same bitmaps; nothing of this program's left behind. */
    CHECK(IIntuition->OpenIntuition(&in, CurrentTime() + 20000 * MS) == 0);
    CHECK(info(in.in_Call) == 0);
    /* Where the pointer is, which nothing moves now, and input's rings
     * opened: hwdb may kill input as soon as display is back. */
    opens = in.in_Call->im_InputOpens;
    input = in.in_Call->im_Input;
    px = ptr_x;
    py = ptr_y;
    if ((r = open_three(readable)))
        return r;
    CHECK(QueryProcess(0, &p1) == 0 && p1.pi_Pages == p0.pi_Pages);
    KPrintF("window: intuition killed: the console showed through, and the windows opened again on "
            "its next self from the same bitmaps");

    /* display killed under intuition: sa's frames go on all the while,
     * and are shown again once it's back. Told at once of its death, by
     * this program's own connection to it. */
    for (until = CurrentTime() + 30000 * MS; !PortGone(&dmp);) {
        CHECK(CurrentTime() < until);
        CHECK(present_one() == 0);
        WaitUntil(1ULL << SIG_DISPLAY, CurrentTime() + 16 * MS);
    }
    ClosePort(&dmp);
    /* While it's away, a frame is safe at once: nothing reads the other
     * buffer. So before it's back (hwdb starts it 10 ms after at least). */
    CHECK(present_one() == 0 && IIntuition->WaitSafe(&sa, CurrentTime() + 1000 * MS) == 0);
    CHECK(display_up(0) != 0);
    for (until = CurrentTime() + 20000 * MS;; nap(16)) {
        CHECK(present_one() == 0);
        away++;
        if (display_up(0) == 0)
            break;
        CHECK(CurrentTime() < until);
    }
    CHECK(present_one() == 0);
    CHECK(on_screen(&sa, sa.w_Frame));
    CHECK(info(in.in_Call) == 0);
    CHECK(screen_is(readable, 64, 48, 200, 120));
    CHECK(screen_is(readable, 304, 226, 160, 100));
    KPrintF("window: display killed under intuition: %lu frames presented while it was away, safe "
            "at once, and shown once it was back", away + 1);

    /* input killed: its ring opened again, the pointer where it was. */
    m = in.in_Call;
    if (input) {
        for (until = CurrentTime() + 30000 * MS; info(m) == 0 && m->im_InputOpens == opens; nap(10))
            CHECK(CurrentTime() < until);
        CHECK(m->im_Input && ptr_x == px && ptr_y == py);
        if (mouse && (r = saved_back()))
            return r;
        /* The mouse is input's again once xhci has told it of it, a
         * little after: until then a move is lost, and asked for again. */
        for (until = CurrentTime() + 10000 * MS; mouse && !point_at(380, 280);)
            CHECK(CurrentTime() < until);
        if (mouse)
            CHECK(clicked_in(&sb, 380, 280));
        KPrintF("window: input killed: its ring opened again, the pointer where it was%s",
                mouse ? ", and a click heard after" : "");
    } else {
        KPrintF("window: input: not exercised across its death: intuition holds no input ring");
    }
    CHECK(IIntuition->CloseWindow(&sc) == 0 && IIntuition->CloseWindow(&sb) == 0 &&
          IIntuition->CloseWindow(&sa) == 0);
    IIntuition->CloseIntuition(&in);
    for (unsigned i = 0; i < 4; i++)
        IGfx->FreeBitMap(sbm[i]);
    return 0;
}

/* --- 9: the desktop, killed under (V8 step 8). --- */

static int64_t hwdb_ask(uint32_t op, uint32_t token)
{
    static struct HwdbMsg m;        /* 4 KiB: not on the stack */

    memset(&m, 0, sizeof m);
    m.hm_Msg.mn_Length = sizeof m;
    m.hm_Op = op;
    m.hm_Token = token;
    return CallPort("hwdb", SIG_HWDB, &m.hm_Msg, CurrentTime() + DRV_HWDB_MS * MS);
}

static uint32_t black;              /* the whole screen's CRC, were it black */
static uint32_t screen_w, screen_h; /* kept: 'in' is cleared while intuition's away */
static unsigned reads;              /* the screen read back while something was dead */

/* The whole screen read back, if it can be: not black. */
static bool lit(bool readable)
{
    uint32_t got;

    if (!readable)
        return true;
    reads++;
    if (read_screen(0, 0, screen_w, screen_h, &got) == 0 && got != black)
        return true;
    KPrintF("window: the screen went dark");
    return false;
}

/* hwdb kills 'what' of the desktop's; the screen is lit for 300 ms at
 * least and until 'n' windows are on it, 20 s at most. */
static int killed_to(bool readable, uint32_t what, uint32_t n)
{
    struct IntuiMsg *m = in.in_Call;
    uint64_t start = CurrentTime();

    CHECK(hwdb_ask(HWDB_TEST_KILL, what) == 0);
    for (;;) {
        CHECK(lit(readable));
        CHECK(info(m) == 0);
        if (m->im_Windows == n && CurrentTime() - start > 300 * MS)
            return 0;
        CHECK(CurrentTime() - start < 20000 * MS);
        nap(10);
    }
}

/* A window of this program's dragged over the Workbench by its title
 * bar, 'n' times: the frames intuition composed, and what each cost. */
static int drag_over(void)
{
    struct IntuiMsg *m = in.in_Call;
    struct BitMap *bm;
    struct Window w;
    uint64_t frames, nanos, pixels;
    int32_t x = 400, y = 300;

    CHECK((bm = IGfx->AllocBitMap(320, 200, PIXFMT_XRGB8888, 0, nullptr)));
    CHECK(OpenWindowTags(IIntuition, &in, &w, { WA_Left, x }, { WA_Top, y }, { WA_Title, (uint64_t)"dragged" },
                         { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET }, { WA_BitMap, (uint64_t)bm }) == 0);
    picture(&w.w_RPort, 320, 200, 0x5b88c2, "dragged");
    CHECK(IIntuition->PresentWindow(&w, nullptr, 0) == 0 && on_screen(&w, 1));
    /* The mouse is input's again a little after input's restart: until
     * then a move is lost, and asked for again. */
    for (uint64_t until = CurrentTime() + 10000 * MS; !point_at(x + 60, y + 12);)
        CHECK(CurrentTime() < until);
    CHECK(info(m) == 0);
    frames = m->im_Frames;
    nanos = m->im_Nanos;
    pixels = m->im_Pixels;
    for (int i = 0; i < 12; i++) {
        int32_t dx = i % 2 ? -100 : 100, left = w.w_Sync->is_LeftEdge;
        uint64_t until = CurrentTime() + 2000 * MS;

        KPrintF("window: send drag %ld 0 left", (int64_t)dx);
        while (w.w_Sync->is_LeftEdge != left + dx)
            if (CurrentTime() > until)
                return failed("the dragged window didn't move");
            else
                nap(5);
        nap(60);                        /* the drag's last frame, and the pace of the log */
    }
    CHECK(info(m) == 0 && m->im_Frames > frames);
    frames = m->im_Frames - frames;
    KPrintF("window: desktop: a window dragged over the Workbench: %lu frames composed, %lu us and %lu pixels "
            "a frame", frames, (m->im_Nanos - nanos) / frames / 1000, (m->im_Pixels - pixels) / frames);
    CHECK(IIntuition->CloseWindow(&w) == 0);
    IGfx->FreeBitMap(bm);
    return 0;
}

static int desktop(bool mouse, bool drag)
{
    static struct wb_icon icons[WB_VOLUMES];
    struct IntuiMsg *m;
    struct IntuiEvent ev;
    struct IntuiPrefs np = {};
    struct InputPrefs ip = {};
    struct MsgPort con;
    struct BitMap *bm;
    struct RastPort rp;
    const uint32_t top = pw_theme_top[1], bottom = pw_theme_bottom[1];    /* Ocean, to be saved */
    const struct TagItem it[] = {
        { PREFS_RepeatDelay, INPUT_DELAY_DEFAULT }, { PREFS_RepeatRate, 20 },
        { PREFS_PointerSpeed, 150 }, { PREFS_PointerAccel, INPUT_ACCEL_DEFAULT }, { TAG_END, 0 },
    };
    const struct TagItem nt[] = {
        { PREFS_Hud, 0 }, { PREFS_PointerSize, INTUI_POINTER_AUTO }, { PREFS_BackdropTop, top },
        { PREFS_BackdropBottom, bottom }, { TAG_END, 0 },
    };
    unsigned n, opens;
    uint64_t until;
    bool readable, input;
    uint32_t got = 0;
    int64_t e;
    int r;

    if ((r = open_lib()))
        return r;
    CHECK(IIntuition->OpenIntuition(&in, CurrentTime() + 20000 * MS) == 0);
    m = in.in_Call;
    CHECK(display_up(0) == 0);
    /* Read back where display composes the screen, with the mouse to
     * move the pointer off what's read. */
    readable = read_screen(0, 0, 1, 1, &got) == 0 && mouse;
    screen_w = in.in_Width;
    screen_h = in.in_Height;
    if (readable) {
        CHECK((bm = IGfx->AllocBitMap(in.in_Width, in.in_Height, PIXFMT_XRGB8888, 0, nullptr)));
        IGfx->InitRastPort(&rp, bm);
        rp.rp_FgPen = 0xff000000;
        IGfx->RectFill(&rp, 0, 0, (int32_t)in.in_Width - 1, (int32_t)in.in_Height - 1);
        black = crc_rect(bm, 0, 0, in.in_Width, in.in_Height);
        IGfx->FreeBitMap(bm);
    }
    CHECK(OpenDosUntil(&wdos, SIG_DOS, CurrentTime() + 5000 * MS) == 0);
    CHECK((n = wb_volumes(&wdos, icons, WB_VOLUMES)) > 0);
    CHECK(IIntuition->GetPrefs(&in, &np) == 0 && np.ip_BackdropTop == INTUI_BACKDROP_TOP);

    /* The Workbench, which hwdb started with intuition. */
    CHECK(windows_are(1, 20000));
    CHECK(!readable || point_at((int32_t)in.in_Width - 1, 0));
    CHECK(desk_is(readable, icons, n, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM, 10000));
    if ((r = killed_to(readable, HWDB_KILL_WORKBENCH, 1)))
        return r;
    CHECK(desk_is(readable, icons, n, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM, 10000));
    KPrintF("window: desktop: the Workbench killed, and back with its %lu icons", (uint64_t)n);

    /* Prefs and a Shell's conwin, hwdb's: the conwin killed, then Prefs. */
    CHECK(hwdb_ask(HWDB_NEW_PREFS, 0) == 0 && windows_are(2, 10000));
    CHECK(hwdb_ask(HWDB_NEW_PREFS, 0) == ERR_EXISTS);
    CHECK(hwdb_ask(HWDB_NEW_SHELL, 0) == 0 && windows_are(3, 10000));
    CHECK(inside_is(readable, sw_crc_ready, 10000, "the Shell's window with its prompt"));
    if ((r = killed_to(readable, HWDB_KILL_SHELL, 2)))
        return r;
    CHECK(ConnectPort(&con, "con.1", SIG_CONWIN, 2, 256) == ERR_NOTFOUND);
    if ((r = killed_to(readable, HWDB_KILL_PREFS, 1)))
        return r;
    CHECK(desk_is(readable, icons, n, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM, 5000));
    CHECK(hwdb_ask(HWDB_NEW_PREFS, 0) == 0 && windows_are(2, 10000));
    KPrintF("window: desktop: a Shell's conwin killed, its window and con.1 gone; Prefs killed, its window "
            "gone, and another started");

    /* Settings saved; then intuition killed under the Workbench and Prefs. */
    CHECK(WritePrefs(&wdos, "input", it) == 0 && WritePrefs(&wdos, "intuition", nt) == 0);
    CHECK(hwdb_ask(HWDB_TEST_KILL, HWDB_KILL_INTUITION) == 0);
    until = CurrentTime() + 10000 * MS;
    while ((e = IIntuition->WaitIntuiEvent(&in, until)) == 0)
        while (IIntuition->GetIntuiEvent(&in, &ev))
            ;
    CHECK(e == ERR_PORT_DIED);
    IIntuition->CloseIntuition(&in);
    for (until = CurrentTime() + 20000 * MS;;) {
        CHECK(lit(readable));
        if (IIntuition->OpenIntuition(&in, CurrentTime() + 20 * MS) == 0)
            break;
        CHECK(CurrentTime() < until);
    }
    m = in.in_Call;
    CHECK(windows_are(1, 20000));
    nap(500);
    CHECK(info(m) == 0 && m->im_Windows == 1);         /* Prefs ended with intuition, and wasn't started again */
    CHECK(IIntuition->GetPrefs(&in, &np) == 0 && np.ip_BackdropTop == top && np.ip_BackdropBottom == bottom);
    CHECK(!readable || point_at((int32_t)in.in_Width - 1, 0));
    CHECK(desk_is(readable, icons, n, top, bottom, 10000));
    KPrintF("window: desktop: intuition killed under the Workbench and Prefs: the Workbench back on its next "
            "self, in the saved backdrop's colours; Prefs not");

    /* input killed: back with the saved settings. */
    opens = m->im_InputOpens;
    input = m->im_Input;
    CHECK(hwdb_ask(HWDB_TEST_KILL, HWDB_KILL_INPUT) == 0);
    for (until = CurrentTime() + 30000 * MS; input && info(m) == 0 && m->im_InputOpens == opens; nap(10))
        CHECK(lit(readable) && CurrentTime() < until);
    CHECK(input_is(INPUT_DELAY_DEFAULT, 20, 150, 5000));
    KPrintF("window: desktop: input killed, and back with the saved settings");

    /* As they were, and the files gone; the Workbench told. */
    ip = (struct InputPrefs){ INPUT_DELAY_DEFAULT, INPUT_RATE_DEFAULT, INPUT_SPEED_DEFAULT, INPUT_ACCEL_DEFAULT };
    np = (struct IntuiPrefs){ 0, INTUI_POINTER_AUTO, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM };
    CHECK(input_prefs(INPUT_SET, &ip) == 0 && IIntuition->SetPrefs(&in, &np) == 0);
    CHECK(DeletePath(&wdos, "SYS:Prefs/Env-Archive/input.prefs") == 0 &&
          DeletePath(&wdos, "SYS:Prefs/Env-Archive/intuition.prefs") == 0);
    CHECK(desk_is(readable, icons, n, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM, 5000));
    CloseDos(&wdos);
    if (readable)
        KPrintF("window: desktop: the screen read back %lu times while something was dead, never dark",
                (uint64_t)reads);

    /* The costs: a window dragged over the Workbench. */
    if (mouse && (r = drag_over()))
        return r;
    if (drag && (r = by_hand()))
        return r;
    IIntuition->CloseIntuition(&in);
    return 0;
}

int main(uint64_t arg)
{
    struct BitMap *bm[3];
    struct AreaInfo ai;
    bool readable;
    int r, input;

    if ((arg & 0x0f) == DESKTOP)
        return desktop(arg & DESKTOP_MOUSE, arg & DRAG);
    if ((arg & 0xff) == STOPPED)
        return stopped();
    if ((arg & 0xff) == KILLED) {
        r = killed((Handle)(arg >> 32));
        KPrintF("window: the client to be killed couldn't show its windows (%ld)", (int64_t)r);
        return r;
    }
    if ((arg & 3) == SURVIVE)
        return survive(arg & SURVIVE_MOUSE);
    if ((r = open_lib()))
        return r;
    if ((r = (int)IIntuition->OpenIntuition(&in, CurrentTime() + 5000 * MS)) != 0)
        KPrintF("window: intuition wouldn't connect: %ld", (int64_t)r);
    CHECK(r == 0);
    CHECK(in.in_Width >= 640 && in.in_Height >= 480);
    CHECK(display_up(0) == 0);
    CHECK(info(in.in_Call) == 0);
    if ((r = two_windows(&readable, bm)))
        return r;
    if (arg & LOOK) {
        KPrintF("window: look: for 20 s, a dark navy backdrop (#182038) where the console was,");
        KPrintF("window: look: a blue window 200x120 at (64, 48) saying \"window a\", a grey one 160x100");
        KPrintF("window: look: at (180, 100) over it saying \"window b\", and the pointer at (%ld, %ld)",
                (int64_t)ptr_x, (int64_t)ptr_y);
        nap(20000);
    }
    if (arg & DRAG && (r = by_hand()))
        return r;
    if ((r = refusals(bm)) || (r = frames(readable)) || (r = paced_both(readable)))
        return r;

    /* 4: closed, and the backdrop where they were; intuition maps their
     * bitmaps while they're open, and not after. */
    for (unsigned i = 0; i < 3; i++)
        CHECK(QueryArea(bm[i]->bm_BaseAddr, &ai) == 0 && ai.ai_Sharers == 2);
    CHECK(IIntuition->CloseWindow(&wb) == 0);
    taken_off(&wb);
    CHECK(screen_is(readable, 32, 32, 416, 240));
    CHECK(IIntuition->CloseWindow(&wa) == 0);
    CHECK(IIntuition->CloseWindow(&wa) == ERR_BADARG);
    taken_off(&wa);
    CHECK(screen_is(readable, 32, 32, 416, 240));
    for (unsigned i = 0; i < 3; i++)
        CHECK(QueryArea(bm[i]->bm_BaseAddr, &ai) == 0 && ai.ai_Sharers == 1);

    input = input_half(readable);
    ends();
    if (input && input != NO_INPUT)
        return input;
    if (!input && ((r = desktop_half(readable)) || (r = shell_half(readable)) || (r = ui_half(readable)) ||
                   (r = workbench_half(readable))))
        return r;
    if ((r = kill_window(readable)))
        return r;

    CHECK(info(in.in_Call) == 0 && in.in_Call->im_Frames > 0);
    KPrintF("window: intuition composed %lu frames, %lu us a megapixel; the screen %s",
            in.in_Call->im_Frames, in.in_Call->im_Nanos * 1000 / (in.in_Call->im_Pixels + 1),
            readable ? "read back as drawn at each step" : "not read back (display's controller)");
    IIntuition->CloseIntuition(&in);
    for (unsigned i = 0; i < 3; i++)
        IGfx->FreeBitMap(bm[i]);
    return input;
}
