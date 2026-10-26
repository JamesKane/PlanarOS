/*
 * servers/conwin/main.c -- a console window: the Shell's, in a window
 * (V8 step 5, decision 7)
 *
 * hwdb starts one when the system menu's Shell is chosen (intuition asks
 * it: HWDB_NEW_SHELL). It publishes the first of "con.1" to "con.8" that
 * isn't taken, opens a window on intuition, and starts C:shell with that
 * console (the Shell's line says it: "SYS:\tcon.1\n"), which its
 * commands are given in turn. It serves what con.0 serves, DrvRequests
 * (planar/drv.h), to whoever connects:
 *   DRV_WRITE  dr_Len bytes of text, to the window (term.h: UTF-8, and
 *              the few control codes and CSI sequences it reads);
 *   DRV_READ   what's been typed, up to dr_Len bytes: answered once a
 *              line has been (Return), or with none at the end of the
 *              input (Ctrl-\). The line's edited here, as the Amiga's
 *              CON: edits it, and echoed as it's typed.
 * A connection has one read waiting at most: a second is ERR_BUSY.
 *
 * Its window, in System Mono 13 px (term.c draws it), has every gadget.
 * Its keys: the characters typed, to the line; Page Up and Page Down a
 * window's lines back and forth in the scrollback, Shift with the arrows
 * a line, Shift with Home and End to either end. Resized, it shows as
 * many cells as fit, and reflows nothing.
 *
 * intuition's events come by the page's word, not a signal: a task of
 * its own (watcher) waits on it and signals this one, which waits for
 * its connections, the Shell, and that. One buffer: a frame is drawn
 * once the last is safe, after everything that came in meanwhile.
 *
 * Endings: the Shell's (EndShell, or its death) closes the window and
 * ends conwin. The close gadget ends both: the Shell is conwin's, so it
 * and its command die with conwin's handle. conwin's own death takes
 * the Shell with it the same way. intuition's death leaves both: the
 * window's opened again on the next intuition, drawn whole from the
 * scrollback, and the Shell never knows.
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/dos.h>
#include <planar/drv.h>
#include <planar/intuition.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

#include "conwin.h"
#include "log.h"
#include "term.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_ACCEPT = 16, SIG_CONN = 17, SIG_EVENT = 18, SIG_CHILD = 19, SIG_TIMER = 20, SIG_DOS = 21;
/* 22 is intuition.library's */
constexpr uint64_t MS = 1000000;
constexpr unsigned CONNS = 8;
constexpr uint32_t IDCMP = IDCMP_VANILLAKEY | IDCMP_RAWKEY | IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE |
                           IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW;

/* Keys' usages (HID's), as IDCMP_RAWKEY has them. */
enum : uint16_t { KEY_HOME = 0x4a, KEY_PGUP = 0x4b, KEY_END = 0x4d, KEY_PGDN = 0x4e, KEY_DOWN = 0x51, KEY_UP = 0x52 };

static struct {
    struct GfxIFace *gfx;
    struct IntuitionIFace *intuition;
    struct Intuition in;
    struct Window win;
    bool up, open;                      /* connected to intuition; the window open */
    uint64_t retry;                     /* when to connect again, intuition gone */
    struct BitMap *bm;
    uint32_t width, height;             /* the window's inside */
    int32_t left, top;
    char title[16];
    struct term t;
    struct term_font f;
    unsigned n;                         /* con.n */
    char name[8];
    Handle port, shell, task;
    struct MsgPort conn[CONNS];
    struct DrvRequest *reading[CONNS];  /* a read waiting for a line */
    unsigned nconns;
    bool timer;
} cw;

static void say(const char *text)
{
    KLog(text);
}

/* --- The watcher: intuition's page, to a signal. --- */

static struct {
    const struct IntuiPage *page;       /* while 'on': the main task's to change */
    uint64_t on, off;                   /* the main task's say; the watcher's: it's stopped reading */
} watch;

static void watcher(uint64_t)
{
    uint64_t seen = 0;

    for (;;) {
        const struct IntuiPage *p;
        uint64_t head;

        if (!__atomic_load_n(&watch.on, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&watch.off, 1, __ATOMIC_RELEASE);
            WakeAddr(&watch.off, 1);
            WaitAddr(&watch.on, 0, DEADLINE_NONE);
            seen = 0;
            continue;
        }
        p = watch.page;
        head = __atomic_load_n(&p->ip_Head, __ATOMIC_ACQUIRE);
        if (head != seen) {
            seen = head;
            Signal(cw.task, 1ULL << SIG_EVENT);
        }
        /* Woken by an event, or at once when intuition dies. */
        WaitAddr(&p->ip_Head, head, CurrentTime() + 250 * MS);
    }
}

static void watch_start(void)
{
    watch.page = cw.in.in_Page;
    __atomic_store_n(&watch.off, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&watch.on, 1, __ATOMIC_RELEASE);
    WakeAddr(&watch.on, 1);
}

/* The watcher no longer reads the page: it may be let go of. */
static void watch_stop(void)
{
    uint64_t until = CurrentTime() + 1000 * MS;

    __atomic_store_n(&watch.on, 0, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&watch.off, __ATOMIC_ACQUIRE) && CurrentTime() < until)
        WaitAddr(&watch.off, 0, until);
}

/* --- The window. --- */

static int64_t open_window(uint64_t deadline)
{
    int64_t e;

    if (!cw.up) {
        if ((e = cw.intuition->OpenIntuition(&cw.in, deadline)) != 0)
            return e;
        cw.up = true;
    }
    if (!cw.bm) {
        /* As many of 80 by 24 cells as the screen has room for. */
        uint32_t room_w = cw.in.in_Width - (uint32_t)cw.left - 2 * INTUI_BORDER;
        uint32_t room_h = cw.in.in_Height - (uint32_t)cw.top - INTUI_TITLEBAR - INTUI_SIZEBORDER;
        uint32_t cols = CONWIN_COLS, rows = CONWIN_ROWS;

        while (cols > CONWIN_MIN_COLS && term_width(&cw.f, cols) > room_w)
            cols--;
        while (rows > CONWIN_MIN_ROWS && term_height(&cw.f, rows) > room_h)
            rows--;
        cw.width = term_width(&cw.f, cols);
        cw.height = term_height(&cw.f, rows);
        if (!(cw.bm = cw.gfx->AllocBitMap(cw.width, cw.height, PIXFMT_XRGB8888, 0, nullptr)))
            return ERR_NOMEM;
        term_resize(&cw.t, cols, rows);
    }
    e = OpenWindowTags(cw.intuition, &cw.in, &cw.win, { WA_Left, (uint64_t)cw.left },
                       { WA_Top, (uint64_t)cw.top }, { WA_Width, cw.width }, { WA_Height, cw.height },
                       { WA_Title, (uint64_t)cw.title },
                       { WA_Flags, WFLG_SIZEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                                   WFLG_ACTIVATE },
                       { WA_IDCMP, IDCMP }, { WA_MinWidth, term_width(&cw.f, CONWIN_MIN_COLS) },
                       { WA_MinHeight, term_height(&cw.f, CONWIN_MIN_ROWS) }, { WA_BitMap, (uint64_t)cw.bm });
    if (e != 0)
        return e;
    cw.open = true;
    cw.t.all = true;
    watch_start();
    return 0;
}

/* intuition went: the window's to be opened again on its next self. */
static void intuition_gone(void)
{
    if (!cw.up)
        return;
    say(cw.open ? "conwin: intuition went: the window's opened again when it's back" : "conwin: intuition went");
    watch_stop();
    if (cw.open) {
        cw.left = cw.win.w_Sync->is_LeftEdge;   /* where it was dragged to */
        cw.top = cw.win.w_Sync->is_TopEdge;
    }
    cw.intuition->CloseIntuition(&cw.in);
    cw.up = cw.open = false;
    cw.retry = CurrentTime() + 50 * MS;
}

static void resize(uint32_t w, uint32_t h)
{
    struct BitMap *bm = cw.gfx->AllocBitMap(w, h, PIXFMT_XRGB8888, 0, nullptr);

    if (!bm)
        return;                         /* shown as it was, cut or with the backdrop past it */
    if (cw.intuition->ChangeWindowBuffers(&cw.win, bm, nullptr) != 0) {
        cw.gfx->FreeBitMap(bm);
        return;
    }
    cw.gfx->FreeBitMap(cw.bm);
    cw.bm = bm;
    cw.width = w;
    cw.height = h;
    term_resize(&cw.t, (w - 2 * TERM_PAD) / cw.f.cw, (h - 2 * TERM_PAD) / cw.f.ch);
}

/* What's changed, drawn and presented, once the last frame is safe. */
static void draw(void)
{
    struct GfxRect r;

    if (!cw.open)
        return;
    if (cw.win.w_Sync->is_Safe < cw.win.w_Frame) {
        cw.timer = true;                /* look again shortly */
        return;
    }
    if (!term_draw(cw.gfx, &cw.f, &cw.win.w_RPort, cw.width, cw.height, &cw.t, &r))
        return;
    if (cw.intuition->PresentWindow(&cw.win, &r, 1) == ERR_PORT_DIED)
        intuition_gone();
}

/* --- Its ending. --- */

[[noreturn]] static void end(int code)
{
    if (cw.open)
        cw.intuition->CloseWindow(&cw.win);
    if (cw.shell)
        CloseHandle(cw.shell);          /* which ends it, and its command */
    ExitProcess(code);
}

static void key(const struct IntuiEvent *ev)
{
    bool shift = ev->iev_Qualifier & IEQ_SHIFT;
    int32_t page = cw.t.rows > 1 ? (int32_t)cw.t.rows - 1 : 1;

    if (ev->iev_Class == IDCMP_VANILLAKEY) {
        /* Ctrl-\, the end of the input: input types Ctrl with letters alone. */
        term_key(&cw.t, ev->iev_Char == '\\' && ev->iev_Qualifier & IEQ_CTRL ? 0x1c : ev->iev_Char);
        return;
    }
    switch (ev->iev_Code) {             /* down, without a character */
    case KEY_PGUP:
        term_scroll(&cw.t, page);
        break;
    case KEY_PGDN:
        term_scroll(&cw.t, -page);
        break;
    case KEY_UP:
        if (shift)
            term_scroll(&cw.t, 1);
        break;
    case KEY_DOWN:
        if (shift)
            term_scroll(&cw.t, -1);
        break;
    case KEY_HOME:
        if (shift)
            term_scroll(&cw.t, INT32_MAX);
        break;
    case KEY_END:
        if (shift)
            term_scroll(&cw.t, INT32_MIN + 1);
        break;
    default:
        break;
    }
}

static void events(void)
{
    struct IntuiEvent ev;

    if (!cw.open)
        return;
    while (cw.intuition->GetIntuiEvent(&cw.in, &ev)) {
        if (ev.iev_Window != cw.win.w_Id)
            continue;
        switch (ev.iev_Class) {
        case IDCMP_VANILLAKEY:
            key(&ev);
            break;
        case IDCMP_RAWKEY:
            if (!(ev.iev_Code & RAWKEY_UP))
                key(&ev);
            break;
        case IDCMP_CLOSEWINDOW:
            end(0);
        case IDCMP_NEWSIZE:
            resize((uint32_t)ev.iev_MouseX, (uint32_t)ev.iev_MouseY);
            break;
        case IDCMP_ACTIVEWINDOW:
            term_set_active(&cw.t, true);
            break;
        case IDCMP_INACTIVEWINDOW:
            term_set_active(&cw.t, false);
            break;
        default:
            break;
        }
    }
    if (PortGone(&cw.in.in_Port))
        intuition_gone();
}

/* --- The console's clients. --- */

static void answer(unsigned c, struct DrvRequest *r, int64_t error)
{
    r->dr_Msg.mn_Error = (int32_t)error;
    ReplyMsgLater(&cw.conn[c], &r->dr_Msg);
}

static void message(unsigned c, struct DrvRequest *r)
{
    char text[sizeof r->dr_Data];
    uint32_t n = r->dr_Len < sizeof text ? r->dr_Len : sizeof text;

    if (r->dr_Op == DRV_WRITE) {
        memcpy(text, r->dr_Data, n);    /* the client's memory: copied before it's read */
        term_write(&cw.t, text, n);
        answer(c, r, 0);
    } else if (r->dr_Op == DRV_READ && !cw.reading[c]) {
        cw.reading[c] = r;              /* answered once there's a line */
    } else {
        answer(c, r, r->dr_Op == DRV_READ ? ERR_BUSY : ERR_BADARG);
    }
}

/* Reads waiting, answered from what's been typed; at the end of the
 * input, each with nothing, once. */
static void reads(void)
{
    bool eof = false;

    for (unsigned c = 0; c < cw.nconns; c++) {
        struct DrvRequest *r = cw.reading[c];
        uint32_t want;

        if (!r || (!cw.t.qlen && !cw.t.eof))
            continue;
        want = r->dr_Len < sizeof r->dr_Data ? r->dr_Len : sizeof r->dr_Data;
        r->dr_Len = cw.t.qlen ? term_read(&cw.t, (char *)r->dr_Data, want) : 0;
        eof |= r->dr_Len == 0;
        cw.reading[c] = nullptr;
        answer(c, r, 0);
    }
    if (eof)
        cw.t.eof = false;
}

static void serve(void)
{
    for (struct MsgPort extra;;) {
        struct MsgPort *mp = cw.nconns < CONNS ? &cw.conn[cw.nconns] : &extra;

        if (AcceptPort(mp, cw.port, SIG_CONN) != 0)
            break;
        if (mp == &extra) {
            ClosePort(&extra);          /* full: told, not left waiting */
        } else {
            cw.reading[cw.nconns] = nullptr;
            cw.nconns++;
        }
    }
    for (unsigned c = 0; c < cw.nconns;) {
        struct DrvRequest *r;

        while ((r = (struct DrvRequest *)GetMsgSized(&cw.conn[c], sizeof *r)))
            message(c, r);
        if (PortGone(&cw.conn[c])) {
            ClosePort(&cw.conn[c]);
            cw.nconns--;
            cw.conn[c] = cw.conn[cw.nconns];
            cw.reading[c] = cw.reading[cw.nconns];
            continue;
        }
        c++;
    }
}

/* --- Starting. --- */

static bool shell_ended(void)
{
    struct ProcessInfo pi;

    return QueryProcess(cw.shell, &pi) != 0 || pi.pi_State >= PROCS_EXITED;
}

static int64_t start_shell(void)
{
    char line[32];
    struct Dos dos;
    int64_t p;

    if ((p = OpenDosUntil(&dos, SIG_DOS, CurrentTime() + 5000 * MS)) != 0)
        return p;
    SNPrintf(line, sizeof line, "SYS:\t%s\n", cw.name);
    p = RunCommand(&dos, "C:shell", line, SIG_CHILD, 0);
    CloseDos(&dos);
    if (p < 0)
        return p;
    cw.shell = (Handle)p;
    return 0;
}

static bool open_libs(void)
{
    struct Library *lib;
    struct TextFont *regular, *bold;

    if (!(lib = IExec->OpenLibrary("gfx.library", 8)) ||
        !(cw.gfx = (struct GfxIFace *)IExec->GetInterface(lib, "main", 1)) || !cw.gfx->OpenTextFont ||
        !(lib = IExec->OpenLibrary("intuition.library", 8)) ||
        !(cw.intuition = (struct IntuitionIFace *)IExec->GetInterface(lib, "main", 1)) ||
        !cw.intuition->ChangeWindowBuffers)
        return false;
    regular = cw.gfx->OpenTextFont(&(struct TextAttr){ "System Mono", 13, 0, 0 });
    bold = cw.gfx->OpenTextFont(&(struct TextAttr){ "System Mono", 13, FSF_BOLD, 0 });
    if (!regular)
        return false;
    term_font_init(cw.gfx, &cw.f, regular, bold);
    return true;
}

int main(uint64_t)
{
    struct term_cell *cells;
    int64_t e;

    cw.task = FindTask(nullptr);
    if (!open_libs()) {
        say("conwin: no gfx.library, intuition.library or System Mono");
        return 1;
    }
    if (!(cells = AllocVec(TERM_LINES * TERM_COLS * sizeof *cells, AREAF_CLEAR))) {
        say("conwin: no memory for the scrollback");
        return 1;
    }
    term_init(&cw.t, cells, CONWIN_COLS, CONWIN_ROWS);
    for (cw.n = 1; cw.n <= CONWIN_MAX; cw.n++) {
        SNPrintf(cw.name, sizeof cw.name, "con.%u", cw.n);
        if ((e = CreatePort(cw.name, SIG_ACCEPT)) >= 0)
            break;
    }
    if (cw.n > CONWIN_MAX) {
        say("conwin: con.1 to con.8 are all taken");
        return 1;
    }
    cw.port = (Handle)e;
    SNPrintf(cw.title, sizeof cw.title, "Shell %u", cw.n + 1);
    cw.left = CONWIN_LEFT + CONWIN_STEP * (int32_t)(cw.n - 1);
    cw.top = CONWIN_TOP + CONWIN_STEP * (int32_t)(cw.n - 1);
    if (CreateTask(watcher, 0) < 0) {
        say("conwin: no task to watch for intuition's events");
        return 1;
    }
    if ((e = open_window(CurrentTime() + 5000 * MS)) != 0) {
        KPrintF("conwin: no window on intuition (%ld)", e);
        return 1;
    }
    if ((e = start_shell()) != 0) {
        KPrintF("conwin: C:shell couldn't be started (%ld)", e);
        end(1);
    }
    for (;;) {
        uint64_t mask = 1ULL << SIG_ACCEPT | 1ULL << SIG_CONN | 1ULL << SIG_EVENT | 1ULL << SIG_CHILD;
        uint64_t at = cw.timer ? CurrentTime() + 4 * MS : !cw.up ? cw.retry : 0;

        if (at) {
            ArmTimer(at, SIG_TIMER);
            WaitPorts(cw.conn, cw.nconns, mask | 1ULL << SIG_TIMER);
            ArmTimer(0, 0);
        } else {
            WaitPorts(cw.conn, cw.nconns, mask);
        }
        cw.timer = false;
        if (shell_ended())
            end(0);
        if (!cw.up && CurrentTime() >= cw.retry && open_window(CurrentTime() + 10 * MS) != 0)
            cw.retry = CurrentTime() + 100 * MS;
        serve();
        events();
        reads();
        draw();
    }
}
