/*
 * servers/intuition/main.c -- intuition: windows composed on the screen
 * (v7-plan steps 3 to 5)
 *
 * planar/intuition.h has the protocol. intuition is display's client:
 * two full-screen XRGB buffers of display's, shown on plane 1 in turn
 * over the console (decision 2), and the pointer's sprite on plane 2.
 * A frame is composed (compose.c) into the buffer not on the screen once
 * the last flip has latched, so one is composed a vblank at most. A
 * second task waits on the sync page's ds_Disp and signals the first,
 * whose sleep is on its ports. The pointer moving is a commit of plane 2
 * alone, at a vblank at most too, with no window composed.
 *
 * A window's buffers are its client's, shared with intuition read-only.
 * intuition describes each one itself (InitBitMap) from what OPEN said,
 * checked against the Area it lies in, so a client's BitMap is never
 * read. Its chrome is intuition's own (chrome.c), two bitmaps around
 * its inside. A window joins the screen, at the front, at its first
 * frame. Composing a frame takes each window's latest: its is_Safe
 * moves then, and its is_Disp when the flip latches. A frame that
 * changed nothing on the screen (a window off it) is safe and shown at
 * once.
 *
 * intuition holds input's ring, read when a third task, waiting on its
 * head, says it moved. Nothing done with what comes waits for a client:
 * the pointer, focus, dragging and the gadgets are intuition's, on each
 * window's last frame, and what the client is told goes to its
 * connection's ring when it has room (events.c). A window whose close
 * gadget gets no CloseWindow within a second is taken off the screen
 * (decision 6).
 *
 * When a client goes, closed or dead, its windows go and intuition lets
 * go of their buffers' Areas. hwdb starts intuition when it's wanted
 * (decision 4): its test now, and in V8 the Workbench; then it keeps it,
 * as it keeps input.
 *
 * display or input may die under intuition, and hwdb starts them again.
 * intuition lets go of what it had of the dead one, and a task waits for
 * its port to be made again (OpenPort's wait), so the clients are served
 * meanwhile. Back, display gets the whole screen composed into two new
 * buffers; while it's away a window of two buffers has its frames taken
 * at once, as nothing is reading the other. input's ring is opened
 * again, the pointer where it was.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/devices/input.h>
#include <planar/intuition.h>
#include <planar/syscalls.h>

#include "chrome.h"
#include "compose.h"
#include "events.h"
#include "log.h"
#include "pointer.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_ACCEPT = 16, SIG_CONN = 17, SIG_LATCH = 18, SIG_DISPLAY = 19;
constexpr int64_t SIG_INPUT = 20, SIG_INPUT_PORT = 21, SIG_TIMER = 22;
constexpr int64_t SIG_DISPLAY_BACK = 23, SIG_INPUT_BACK = 24;
constexpr int64_t SIG_AWAIT = 16;               /* an awaiting task's own, for its connection */
constexpr unsigned CONNS = 16;
constexpr uint64_t MS = 1000000;
constexpr uint64_t CALL_DEADLINE = 1000 * MS;   /* for display's and input's answers (V6 step 7) */
constexpr uint64_t CLOSE_DEADLINE = 1000 * MS;  /* for CloseWindow, after the close gadget */

/* A client's connection: its task, its page, and its events. */
struct client {
    Handle task;
    Handle page_area;               /* 0 until INTUI_EVENTS or its first window */
    struct IntuiPage *page;
    uint32_t lines;                 /* a bit for each sync line in use */
    const struct IntuiAck *ack;     /* the client's, from INTUI_EVENTS: nullptr, no events */
    uint64_t ack_area;              /* the Area it lies in, as mapped here */
    struct ie_queue q;              /* events it hasn't room for yet */
};

struct window {
    bool used, shown;
    bool removed;                   /* taken off for not closing: CLOSE frees it */
    uint8_t conn, line;
    uint32_t buffers, cur;
    uint32_t idcmp;
    struct ic_window icw;           /* part[0] its inside, the rest its chrome */
    struct chrome ch;
    int32_t ix, iy;                 /* the inside's top left, in the window */
    uint32_t iw, ih;
    struct BitMap *top, *side;      /* the chrome's: intuition's own */
    struct BitMap bm[INTUI_BUFFERS];
    uint64_t area[INTUI_BUFFERS];   /* the Areas they lie in, as mapped here */
    uint64_t frame;                 /* presented last */
    uint64_t composed;              /* taken by the last frame composed */
    uint64_t latching;              /* in the flip waiting to latch */
    uint64_t close_by;              /* its close gadget clicked: CloseWindow by then */
    char title[INTUI_TITLE];
};

static struct {
    Handle port, task;
    struct MsgPort mp[CONNS];
    struct client cl[CONNS];
    unsigned nconns;
    struct window win[IC_WINDOWS];
    struct ic_screen screen;
    const struct GfxIFace *gfx;

    /* display, and what intuition shows on it: ds nullptr while it's away. */
    struct MsgPort dp;
    struct DisplayMsg *dm;
    const struct DisplaySync *ds;
    uint32_t width, height;
    uint32_t buf[2], pointer;
    struct BitMap sbm[2], pbm;
    unsigned back;
    int32_t px, py;
    bool flipping, pointer_moved;
    uint64_t flip_seq;

    /* input, and what's done with it: ring nullptr while there's none. */
    struct MsgPort ip;
    const struct InputRing *ring;
    uint64_t itail;
    uint32_t input_opens;           /* rings opened, for INFO */
    uint16_t buttons;               /* IEQ_*BUTTON held */
    struct window *active;
    struct window *drag;            /* by its title bar, the pointer at (dx, dy) of it */
    int32_t drag_dx, drag_dy;
    struct window *press;           /* a gadget of it held down, ch.pressed's kind */
    unsigned pressing;
    struct window *button_to[3];    /* each button's press went to its inside */

    uint64_t frames, nanos;
} it;

static void say(const char *s)
{
    KLog(s);
}

static void nap(uint64_t ms)
{
    static uint64_t never;              /* a word nobody wakes */

    WaitAddr(&never, 0, CurrentTime() + ms * MS);
}

/* --- The tasks that wait, so the first needn't. --- */

/*
 * A word another process moves (display's ds_Disp, input's ir_Head),
 * waited on by a task of its own, which signals the first task each
 * time it does. watch_set() gives it another, or none, and returns only
 * once it has stopped reading the old one, which may then be let go.
 */
struct watch {
    const volatile uint64_t *word;  /* the first task's: nullptr, none */
    const volatile uint64_t *on;    /* the watcher's: the word it's about to wait on */
    uint64_t gen;                   /* moved with 'word', and woken */
    uint64_t sig;
};

static struct watch latch_w = { .sig = 1ULL << SIG_LATCH }, input_w = { .sig = 1ULL << SIG_INPUT };

static void watcher(uint64_t arg)
{
    struct watch *w = (struct watch *)arg;

    for (;;) {
        uint64_t g = __atomic_load_n(&w->gen, __ATOMIC_SEQ_CST), v;
        const volatile uint64_t *word = __atomic_load_n(&w->word, __ATOMIC_SEQ_CST);

        __atomic_store_n(&w->on, word, __ATOMIC_SEQ_CST);
        if (!word) {
            WaitAddr(&w->gen, g, 0);
            continue;
        }
        if (__atomic_load_n(&w->word, __ATOMIC_SEQ_CST) != word)
            continue;                   /* changed meanwhile: the old one may be going */
        v = *word;
        Signal(it.task, w->sig);
        WaitAddr(word, v, 0);
    }
}

static void watch_set(struct watch *w, const volatile uint64_t *word)
{
    const volatile uint64_t *old = w->word;

    __atomic_store_n(&w->word, word, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&w->gen, 1, __ATOMIC_SEQ_CST);
    WakeAddr(&w->gen, 1);
    /* Woken until it has looked again: it may be about to sleep on 'old'. */
    while (old && __atomic_load_n(&w->on, __ATOMIC_SEQ_CST) == old) {
        WakeAddr(old, UINT32_MAX);
        nap(1);
    }
}

/*
 * A server's port, waited for by a task of its own once asked, which
 * signals the first task when it's there. It has to connect to find it;
 * that connection is closed at once, and the first task makes its own,
 * whose signals are its own.
 */
struct await {
    const char *name;
    int64_t sig;
    uint64_t asked;
};

static struct await display_a = { "display.0", SIG_DISPLAY_BACK, 0 }, input_a = { "input", SIG_INPUT_BACK, 0 };

static void awaiter(uint64_t arg)
{
    struct await *a = (struct await *)arg;
    uint64_t done = 0;

    for (;;) {
        struct MsgPort mp;
        uint64_t asked;

        while ((asked = __atomic_load_n(&a->asked, __ATOMIC_ACQUIRE)) == done)
            WaitAddr(&a->asked, done, 0);
        done = asked;
        if (ConnectPortUntil(&mp, a->name, SIG_AWAIT, 2, 256, DEADLINE_NONE) == 0)
            ClosePort(&mp);
        Signal(it.task, 1ULL << a->sig);
    }
}

static void await(struct await *a)
{
    __atomic_fetch_add(&a->asked, 1, __ATOMIC_RELEASE);
    WakeAddr(&a->asked, 1);
}

/* --- display. --- */

static int64_t display(uint32_t op)
{
    int64_t e;

    it.dm->dm_Msg.mn_Length = sizeof *it.dm;
    it.dm->dm_Op = op;
    it.dm->dm_Task = it.task;
    e = DoMsgUntil(&it.dp, &it.dm->dm_Msg, CurrentTime() + CALL_DEADLINE);
    return e ? e : it.dm->dm_Msg.mn_Error;
}

static int64_t alloc(uint32_t w, uint32_t h, uint32_t format, uint32_t flags, uint32_t *id, struct BitMap *bm)
{
    int64_t e;

    memset(it.dm, 0, sizeof *it.dm);
    it.dm->dm_Width = w;
    it.dm->dm_Height = h;
    it.dm->dm_Format = format;
    it.dm->dm_Flags = flags;
    if ((e = display(DISPLAY_ALLOC)) != 0)
        return e;
    *id = it.dm->dm_Buffer;
    return it.gfx->InitBitMap(bm, (void *)it.dm->dm_Address, w, h, it.dm->dm_Stride,
                              format == DISPLAY_XRGB8888 ? PIXFMT_XRGB8888 : PIXFMT_ARGB8888)
               ? 0
               : ERR_BADARG;
}

/* What intuition had of display, let go: its buffers and its sync
 * page, once the watcher has stopped reading it. */
static void screen_down(void)
{
    watch_set(&latch_w, nullptr);
    for (unsigned b = 0; b < 2; b++)
        if (it.sbm[b].bm_BaseAddr)
            ReleaseArea(it.sbm[b].bm_BaseAddr);
    if (it.pbm.bm_BaseAddr)
        ReleaseArea(it.pbm.bm_BaseAddr);
    if (it.ds)
        ReleaseArea(it.ds);
    ClosePort(&it.dp);
    it.sbm[0] = it.sbm[1] = it.pbm = (struct BitMap){ 0 };
    it.ds = nullptr;
    it.dm = nullptr;
    it.flipping = false;
}

/* display's screen, connected to by 'deadline': at the first, the
 * screen's size and the pointer at its middle; after, all of the screen
 * composed again. ERR_BADARG if display came back with another size. */
static int64_t screen_up(uint64_t deadline)
{
    const struct DisplaySync *ds;
    bool first = !it.width;
    int64_t e;

    if ((e = ConnectPortUntil(&it.dp, "display.0", SIG_DISPLAY, 2, 2 * sizeof *it.dm, deadline)) != 0)
        return e;
    if (!(it.dm = AllocMsg(&it.dp, sizeof *it.dm)))
        return ERR_NOMEM;
    memset(it.dm, 0, sizeof *it.dm);
    if ((e = display(DISPLAY_INFO)) != 0)
        return e;
    it.ds = ds = (const struct DisplaySync *)it.dm->dm_Sync;    /* mapped: screen_down's */
    if (!first && (it.dm->dm_Width != it.width || it.dm->dm_Height != it.height))
        return ERR_BADARG;
    it.width = it.dm->dm_Width;
    it.height = it.dm->dm_Height;
    if (it.dm->dm_Count < 3 || !(it.dm->dm_Plane[1].di_Formats & 1u << DISPLAY_XRGB8888) ||
        it.dm->dm_Plane[1].di_MaxWidth < it.width || it.dm->dm_Plane[1].di_MaxHeight < it.height ||
        !(it.dm->dm_Plane[2].di_Formats & 1u << DISPLAY_ARGB8888))
        return ERR_NOSYS;
    for (unsigned b = 0; b < 2; b++)
        if ((e = alloc(it.width, it.height, DISPLAY_XRGB8888, 0, &it.buf[b], &it.sbm[b])) != 0)
            return e;
    if ((e = alloc(POINTER_SIZE, POINTER_SIZE, DISPLAY_ARGB8888, 0, &it.pointer, &it.pbm)) != 0)
        return e;
    for (uint32_t y = 0; y < POINTER_SIZE; y++)
        for (uint32_t x = 0; x < POINTER_SIZE; x++)
            ((uint32_t *)((uint8_t *)it.pbm.bm_BaseAddr + y * it.pbm.bm_BytesPerRow))[x] =
                pointer_pixel(x, y);
    if (first) {
        it.px = (int32_t)it.width / 2;
        it.py = (int32_t)it.height / 2;
        ic_init(&it.screen, it.gfx, it.width, it.height);
    } else {
        ic_damage(&it.screen, (struct GfxRect){ 0, 0, (int32_t)it.width - 1, (int32_t)it.height - 1 });
    }
    it.back = 0;
    it.pointer_moved = true;
    watch_set(&latch_w, &ds->ds_Disp);
    return 0;
}

/* display went: what's shown again when it's back. */
static void display_lost(void)
{
    say("intuition: display went: the screen composed again when it's back");
    screen_down();
    await(&display_a);
}

static void display_back(void)
{
    int64_t e = screen_up(0);

    if (e == 0) {
        say("intuition: display is back: the screen composed again");
        return;
    }
    screen_down();
    if (e == ERR_BADARG || e == ERR_NOSYS) {
        say("intuition: display came back with another screen");
        ExitProcess(5);
    }
    await(&display_a);              /* gone again, or not yet there */
}

/* --- Frames. --- */

static void set_word(volatile uint64_t *w, uint64_t v)
{
    __atomic_store_n(w, v, __ATOMIC_RELEASE);
    WakeAddr(w, UINT32_MAX);
}

static struct IntuiSync *line_of(const struct window *w)
{
    return &it.cl[w->conn].page->ip_Sync[w->line];
}

/* Each window's latest frame taken: its other buffers are free. */
static void taken(void)
{
    for (unsigned i = 0; i < IC_WINDOWS; i++) {
        struct window *w = &it.win[i];

        if (w->used && w->frame > w->composed) {
            w->composed = w->frame;
            set_word(&line_of(w)->is_Safe, w->composed);
        }
        if (w->used)
            w->latching = w->composed;
    }
}

static void shown(uint64_t at)
{
    for (unsigned i = 0; i < IC_WINDOWS; i++) {
        struct window *w = &it.win[i];

        if (w->used && w->latching > line_of(w)->is_Disp) {
            line_of(w)->is_DispTime = at;
            set_word(&line_of(w)->is_Disp, w->latching);
        }
    }
}

static bool unsafe(void)
{
    for (unsigned i = 0; i < IC_WINDOWS; i++)
        if (it.win[i].used && it.win[i].frame > it.win[i].composed)
            return true;
    return false;
}

/* display away: a window of two buffers has its frame taken, as
 * nothing reads the other; one of one waits for display. */
static void taken_away(void)
{
    for (unsigned i = 0; i < IC_WINDOWS; i++) {
        struct window *w = &it.win[i];

        if (w->used && w->buffers > 1 && w->frame > w->composed) {
            w->composed = w->frame;
            set_word(&line_of(w)->is_Safe, w->composed);
        }
    }
}

static void frame(void)
{
    bool compose;
    uint64_t t;
    int64_t e;
    unsigned n = 0;

    if (!it.ds) {
        taken_away();
        return;
    }
    if (it.flipping)
        return;
    compose = ic_dirty(&it.screen);
    if (!compose && unsafe()) {         /* nothing on the screen changed */
        taken();
        shown(it.ds->ds_Time);
    }
    if (!compose && !it.pointer_moved)
        return;
    memset(it.dm, 0, sizeof *it.dm);
    if (compose) {
        t = CurrentTime();
        ic_compose(&it.screen, &it.sbm[it.back]);
        it.nanos += CurrentTime() - t;
        it.frames++;
        taken();
        it.dm->dm_Set[n++] = (struct DisplayPlane){ 1, it.buf[it.back], 0, 0 };
    }
    it.dm->dm_Set[n++] = (struct DisplayPlane){ 2, it.pointer, it.px, it.py };
    it.dm->dm_Count = n;
    if ((e = display(DISPLAY_COMMIT)) == ERR_PORT_DIED) {
        display_lost();
        return;
    }
    if (e != 0) {
        say("intuition: display refused a frame");
        ExitProcess(e == ERR_TIMEOUT ? 3 : 4);
    }
    it.pointer_moved = false;
    it.flip_seq = it.dm->dm_Seq;
    it.flipping = true;
    if (compose)
        it.back ^= 1;
}

static void latch(void)
{
    if (it.ds && it.flipping && it.ds->ds_Disp >= it.flip_seq) {
        it.flipping = false;
        shown(it.ds->ds_Time);
    }
}

/* --- Windows. --- */

static struct window *of(struct ic_window *icw)
{
    return icw ? (struct window *)((char *)icw - offsetof(struct window, icw)) : nullptr;
}

static uint32_t id_of(const struct window *w)
{
    return (uint32_t)(w - it.win) + 1;
}

static struct window *find(unsigned c, uint32_t id)
{
    struct window *w;

    if (id == 0 || id > IC_WINDOWS)
        return nullptr;
    w = &it.win[id - 1];
    return w->used && w->conn == c ? w : nullptr;
}

/* Let go of the Area at 'area' unless a window's buffer or a client's
 * IntuiAck lies in it. */
static void release(uint64_t area, const struct window *except)
{
    if (!area)
        return;
    for (unsigned i = 0; i < IC_WINDOWS; i++) {
        const struct window *w = &it.win[i];

        if (w != except && w->used)
            for (unsigned b = 0; b < w->buffers; b++)
                if (w->area[b] == area)
                    return;
    }
    for (unsigned c = 0; c < it.nconns; c++)
        if (it.cl[c].ack_area == area)
            return;
    ReleaseArea((const void *)area);
}

/* The Area at 'address', shared with intuition by another process and
 * readable, holding 'size' bytes from there: its address, or 0. */
static uint64_t shared_area(uint64_t address, uint64_t size)
{
    struct AreaInfo ai;

    if (QueryArea((const void *)address, &ai) != 0 || ai.ai_Area != 0 || ai.ai_Sharers < 2 ||
        !(ai.ai_Rights & AREAR_READ))
        return 0;       /* not shared with intuition: not mapped, its own, or the kernel's */
    return address + size <= (uint64_t)ai.ai_Address + ai.ai_Size ? (uint64_t)ai.ai_Address : 0;
}

/* A buffer as OPEN describes it: in an Area another process shared
 * with intuition, readable, and holding all of it. */
static int64_t buffer(const struct IntuiBuffer *ib, uint32_t w, uint32_t h, struct BitMap *bm,
                      uint64_t *area)
{
    if (!(*area = shared_area(ib->ib_Address, 1)))
        return ERR_RIGHTS;
    if (ib->ib_Width < w || ib->ib_Height < h ||
        !it.gfx->InitBitMap(bm, (void *)ib->ib_Address, ib->ib_Width, ib->ib_Height,
                            ib->ib_BytesPerRow, ib->ib_Format))
        return ERR_BADARG;
    return shared_area(ib->ib_Address,
                       (uint64_t)ib->ib_BytesPerRow * (ib->ib_Height - 1) + (uint64_t)ib->ib_Width * 4)
               ? 0
               : ERR_BADARG;
}

static int64_t page_up(unsigned c, Handle task)
{
    struct client *cl = &it.cl[c];
    int64_t h, e;
    void *at;

    if (cl->page_area)
        return 0;
    if ((h = AllocArea(sizeof *cl->page, AREAF_CLEAR, &at)) < 0)
        return h;
    if ((e = ShareArea((Handle)h, task, AREAR_READ)) != 0 && e != ERR_EXISTS) {
        FreeArea((Handle)h);
        return e;
    }
    cl->page_area = (Handle)h;
    cl->page = at;
    cl->task = task;
    return 0;
}

/* --- What a window's client hears. --- */

static void post(struct window *w, uint32_t cls, uint16_t code, uint16_t qual, uint32_t ch,
                 uint64_t time)
{
    struct client *cl = &it.cl[w->conn];
    struct IntuiEvent e = {
        .iev_Time = time, .iev_Class = cls, .iev_Window = id_of(w), .iev_Code = code,
        .iev_Qualifier = (uint16_t)(qual | it.buttons), .iev_Char = ch,
        .iev_MouseX = it.px - (w->icw.x + w->ix), .iev_MouseY = it.py - (w->icw.y + w->iy),
    };

    if (w->idcmp & cls && !w->removed && cl->page)
        ie_push(&cl->q, &e, &cl->page->ip_Lost);
}

static void flush_events(void)
{
    for (unsigned c = 0; c < it.nconns; c++) {
        struct client *cl = &it.cl[c];

        if (cl->ack && cl->q.n && ie_flush(&cl->q, cl->page, cl->ack->ia_Tail))
            WakeAddr(&cl->page->ip_Head, UINT32_MAX);
    }
}

/* --- The chrome, and the windows' places. --- */

static void redraw(struct window *w)
{
    if (!w->top)
        return;
    chrome_draw(it.gfx, w->top, w->side, &w->ch);
    if (w->shown)
        for (unsigned k = 1; k < w->icw.nparts; k++)
            ic_damage_window(&it.screen, &w->icw, w->icw.part[k].r);
}

static void activate(struct window *w, uint64_t time)
{
    struct window *was = it.active;

    if (was == w)
        return;
    it.active = w;
    if (was) {
        was->ch.active = false;
        redraw(was);
        post(was, IDCMP_INACTIVEWINDOW, 0, 0, 0, time);
    }
    if (w) {
        w->ch.active = true;
        redraw(w);
        post(w, IDCMP_ACTIVEWINDOW, 0, 0, 0, time);
    }
}

static void move_to(struct window *w, int32_t x, int32_t y)
{
    if (x == w->icw.x && y == w->icw.y)
        return;
    ic_move(&it.screen, &w->icw, x, y);
    line_of(w)->is_LeftEdge = x;
    line_of(w)->is_TopEdge = y;
}

/* Nothing of intuition's points at 'w' any more. */
static void forget(struct window *w)
{
    if (it.active == w)
        it.active = nullptr;
    if (it.drag == w)
        it.drag = nullptr;
    if (it.press == w)
        it.press = nullptr;
    for (unsigned b = 0; b < 3; b++)
        if (it.button_to[b] == w)
            it.button_to[b] = nullptr;
}

/* Off the screen, its buffers and its chrome let go. */
static void take_off(struct window *w)
{
    uint64_t area[INTUI_BUFFERS];

    forget(w);
    if (w->shown)
        ic_remove(&it.screen, &w->icw);
    w->shown = false;
    for (unsigned b = 0; b < INTUI_BUFFERS; b++) {
        area[b] = w->area[b];
        w->area[b] = 0;
    }
    for (unsigned b = 0; b < w->buffers; b++)
        release(area[b], w);
    w->buffers = 0;
    w->icw.nparts = 0;
    if (w->top)
        it.gfx->FreeBitMap(w->top);
    if (w->side)
        it.gfx->FreeBitMap(w->side);
    w->top = w->side = nullptr;
}

/* Closing late (decision 6): its slot and sync line are kept for its
 * CloseWindow, and its WaitSafe goes on, to the next present's refusal. */
static void remove_late(struct window *w)
{
    KPrintF("intuition: window %lu wasn't closed within %lu ms of its close gadget: "
            "taken off the screen", (uint64_t)id_of(w), CLOSE_DEADLINE / MS);
    take_off(w);
    w->removed = true;
    w->close_by = 0;
    w->composed = w->frame;
    set_word(&line_of(w)->is_Safe, UINT64_MAX);
}

static void close_late(void)
{
    uint64_t now = CurrentTime(), next = 0;

    for (unsigned i = 0; i < IC_WINDOWS; i++) {
        struct window *w = &it.win[i];

        if (!w->used || !w->close_by)
            continue;
        if (w->close_by <= now)
            remove_late(w);
        else if (!next || w->close_by < next)
            next = w->close_by;
    }
    ArmTimer(next, SIG_TIMER);
}

static int64_t open_window(unsigned c, const struct IntuiMsg *q, struct IntuiMsg *m)
{
    struct window *w = nullptr, n = { .used = true, .conn = (uint8_t)c };
    struct client *cl = &it.cl[c];
    int64_t e = 0;
    unsigned line;

    for (unsigned i = 0; i < IC_WINDOWS && !w; i++)
        if (!it.win[i].used)
            w = &it.win[i];
    for (line = 0; line < INTUI_WINDOWS && cl->lines & 1u << line; line++)
        ;
    if (q->im_Buffers < 1 || q->im_Buffers > INTUI_BUFFERS || !q->im_Width || !q->im_Height ||
        q->im_Width > GFX_MAX_SIZE || q->im_Height > GFX_MAX_SIZE ||
        q->im_Left < -(int32_t)GFX_MAX_SIZE || q->im_Left > (int32_t)GFX_MAX_SIZE ||
        q->im_Top < -(int32_t)GFX_MAX_SIZE || q->im_Top > (int32_t)GFX_MAX_SIZE)
        e = ERR_BADARG;
    for (unsigned b = 0; b < q->im_Buffers && b < INTUI_BUFFERS && !e; b++)
        e = buffer(&q->im_Buffer[b], q->im_Width, q->im_Height, &n.bm[b], &n.area[b]);
    if (!e && (!w || line == INTUI_WINDOWS))
        e = ERR_FULL;
    if (!e)
        e = page_up(c, q->im_Task);
    chrome_size(q->im_Flags, q->im_Width, q->im_Height, &n.ch, &n.ix, &n.iy);
    if (!e && !(q->im_Flags & WFLG_BORDERLESS) &&
        (!(n.top = it.gfx->AllocBitMap(n.ch.w, INTUI_TITLEBAR + INTUI_BORDER, PIXFMT_XRGB8888, 0,
                                       nullptr)) ||
         !(n.side = it.gfx->AllocBitMap(2 * INTUI_BORDER, q->im_Height, PIXFMT_XRGB8888, 0,
                                        nullptr))))
        e = ERR_NOMEM;
    if (e) {
        /* Refused: what was shared for it goes back, unless in use. */
        for (unsigned b = 0; b < INTUI_BUFFERS; b++)
            release(n.area[b], nullptr);
        if (n.top)
            it.gfx->FreeBitMap(n.top);
        return e;
    }
    n.line = (uint8_t)line;
    n.buffers = q->im_Buffers;
    n.idcmp = q->im_IDCMP;
    n.iw = q->im_Width;
    n.ih = q->im_Height;
    n.icw = (struct ic_window){ q->im_Left, q->im_Top, n.ch.w, n.ch.h, 0, { 0 } };
    memcpy(n.title, q->im_Title, INTUI_TITLE);
    n.title[INTUI_TITLE - 1] = 0;
    *w = n;
    w->ch.title = w->title;
    w->icw.part[0] = (struct ic_part){ { w->ix, w->iy, w->ix + (int32_t)w->iw - 1,
                                         w->iy + (int32_t)w->ih - 1 }, &w->bm[0], 0, 0 };
    if (w->top)
        chrome_draw(it.gfx, w->top, w->side, &w->ch);
    cl->lines |= 1u << line;
    *line_of(w) = (struct IntuiSync){ .is_LeftEdge = q->im_Left, .is_TopEdge = q->im_Top };
    m->im_Window = id_of(w);
    m->im_Sync = (uint64_t)line_of(w);
    return 0;
}

static void close_window(struct window *w)
{
    take_off(w);
    it.cl[w->conn].lines &= ~(1u << w->line);
    *w = (struct window){ 0 };
}

static int64_t present(unsigned c, const struct IntuiMsg *q)
{
    struct window *w = find(c, q->im_Window);

    if (!w || w->removed)
        return ERR_NOTFOUND;
    if (q->im_Index >= w->buffers || q->im_Count > INTUI_RECTS || q->im_Frame <= w->frame)
        return ERR_BADARG;
    for (unsigned i = 0; i < q->im_Count; i++) {
        const struct GfxRect *r = &q->im_Rect[i];

        if (r->MinX < 0 || r->MinY < 0 || r->MinX > r->MaxX || r->MinY > r->MaxY ||
            r->MaxX >= (int64_t)w->iw || r->MaxY >= (int64_t)w->ih)
            return ERR_BADARG;
    }
    w->cur = q->im_Index;
    w->frame = q->im_Frame;
    w->icw.part[0].bm = &w->bm[w->cur];
    if (!w->shown) {
        chrome_parts(&w->ch, w->top, w->side, &w->icw);
        if (!(w->shown = ic_add(&it.screen, &w->icw))) {
            w->icw.nparts = 0;
            return ERR_FULL;
        }
        if (w->ch.flags & WFLG_ACTIVATE)
            activate(w, CurrentTime());
        return 0;
    }
    if (!q->im_Count)
        ic_damage_window(&it.screen, &w->icw, w->icw.part[0].r);
    for (unsigned i = 0; i < q->im_Count; i++)
        ic_damage_window(&it.screen, &w->icw,
                         (struct GfxRect){ w->ix + q->im_Rect[i].MinX, w->iy + q->im_Rect[i].MinY,
                                           w->ix + q->im_Rect[i].MaxX, w->iy + q->im_Rect[i].MaxY });
    return 0;
}

/* The client's IntuiAck, and its page with the ring. */
static int64_t events(unsigned c, const struct IntuiMsg *q, struct IntuiMsg *m)
{
    struct client *cl = &it.cl[c];
    uint64_t area;
    int64_t e;

    if (cl->ack)
        return ERR_EXISTS;
    if (q->im_Ack % 8 || !(area = shared_area(q->im_Ack, sizeof *cl->ack)))
        return ERR_RIGHTS;
    if ((e = page_up(c, q->im_Task)) != 0) {
        release(area, nullptr);
        return e;
    }
    cl->ack = (const struct IntuiAck *)q->im_Ack;
    cl->ack_area = area;
    m->im_Page = (uint64_t)cl->page;
    return 0;
}

static void message(unsigned c, struct IntuiMsg *m)
{
    static struct IntuiMsg q;
    struct window *w;
    int64_t e = 0;

    memcpy(&q, m, sizeof q);
    switch (q.im_Op) {
    case INTUI_INFO:
        m->im_Server = it.task;
        m->im_Width = it.width;
        m->im_Height = it.height;
        m->im_Frames = it.frames;
        m->im_Pixels = it.screen.painted;
        m->im_Nanos = it.nanos;
        m->im_PointerX = it.px;
        m->im_PointerY = it.py;
        m->im_Active = it.active ? id_of(it.active) : 0;
        m->im_Windows = it.screen.nz;
        m->im_Input = it.ring != nullptr;
        m->im_InputOpens = it.input_opens;
        break;
    case INTUI_OPEN:
        e = open_window(c, &q, m);
        break;
    case INTUI_CLOSE:
        if ((w = find(c, q.im_Window)))
            close_window(w);
        else
            e = ERR_NOTFOUND;
        break;
    case INTUI_PRESENT:
        e = present(c, &q);
        break;
    case INTUI_EVENTS:
        e = events(c, &q, m);
        break;
    default:
        e = ERR_BADARG;
    }
    m->im_Msg.mn_Error = (int32_t)e;
    ReplyMsgLater(&it.mp[c], &m->im_Msg);
}

/* Connection c went: its windows and its page go, and the last
 * connection takes its place. */
static void gone(unsigned c)
{
    unsigned last = it.nconns - 1;
    uint64_t ack = it.cl[c].ack_area;

    for (unsigned i = 0; i < IC_WINDOWS; i++)
        if (it.win[i].used && it.win[i].conn == c)
            close_window(&it.win[i]);
    it.cl[c].ack = nullptr;
    it.cl[c].ack_area = 0;
    release(ack, nullptr);
    if (it.cl[c].page_area) {
        UnshareArea(it.cl[c].page_area, it.cl[c].task);
        FreeArea(it.cl[c].page_area);
    }
    ClosePort(&it.mp[c]);
    if (c != last) {
        it.mp[c] = it.mp[last];
        it.cl[c] = it.cl[last];
        for (unsigned i = 0; i < IC_WINDOWS; i++)
            if (it.win[i].used && it.win[i].conn == last)
                it.win[i].conn = (uint8_t)c;
    }
    it.cl[last] = (struct client){ 0 };
    it.nconns--;
}

/* --- input. --- */

/* input's ring, by 'deadline': 0, or why not (ERR_BUSY: another holds it). */
static int64_t ring_up(uint64_t deadline)
{
    struct InputMsg *m;
    int64_t e;

    if ((e = ConnectPortUntil(&it.ip, "input", SIG_INPUT_PORT, 2, 2 * sizeof *m, deadline)) != 0)
        return e;
    if (!(m = AllocMsg(&it.ip, sizeof *m))) {
        ClosePort(&it.ip);
        return ERR_NOMEM;
    }
    memset(m, 0, sizeof *m);
    m->im_Msg.mn_Length = sizeof *m;
    m->im_Op = INPUT_OPEN;
    m->im_Task = it.task;
    if ((e = DoMsgUntil(&it.ip, &m->im_Msg, CurrentTime() + CALL_DEADLINE)) != 0 ||
        (e = m->im_Msg.mn_Error) != 0) {
        ClosePort(&it.ip);
        return e;
    }
    it.ring = (const struct InputRing *)m->im_Ring;
    it.itail = it.ring->ir_Head;
    it.input_opens++;
    watch_set(&input_w, &it.ring->ir_Head);
    return 0;
}

/* input's ring, unless another program holds it: the Shell may
 * (decision 4), and then intuition goes on without it. A holder that
 * died, as intuition's last self may have, is let go of by input soon
 * after, so at the start ERR_BUSY is tried again for a second. Without
 * input, its port is waited for. */
static void input_up(uint64_t deadline)
{
    uint64_t until = CurrentTime() + 1000 * MS;
    int64_t e;

    while ((e = ring_up(deadline)) == ERR_BUSY && deadline && CurrentTime() < until)
        nap(50);
    if (e == 0) {
        if (it.input_opens > 1)
            say("intuition: input is back: its ring opened again, the pointer where it was");
        return;
    }
    if (e == ERR_BUSY) {
        say("intuition: input's ring is another's: no pointer or keys");
        return;
    }
    if (it.input_opens == 0 && deadline)
        say("intuition: no input: no pointer or keys until it comes");
    await(&input_a);
}

/* input went: nothing is held down any more. */
static void input_lost(void)
{
    say("intuition: input went: its ring opened again when it's back");
    watch_set(&input_w, nullptr);
    ReleaseArea(it.ring);
    ClosePort(&it.ip);
    it.ring = nullptr;
    it.buttons = 0;
    it.drag = nullptr;
    for (unsigned b = 0; b < 3; b++)
        it.button_to[b] = nullptr;
    if (it.press) {
        it.press->ch.pressed = CH_NONE;
        redraw(it.press);
        it.press = nullptr;
    }
    await(&input_a);
}

static int32_t clamp(int64_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

static void pointer(const struct InputEvent *ev)
{
    int32_t x = clamp((int64_t)it.px + ev->ie_X, 0, (int32_t)it.width - 1);
    int32_t y = clamp((int64_t)it.py + ev->ie_Y, 0, (int32_t)it.height - 1);

    if (x == it.px && y == it.py)
        return;
    it.px = x;
    it.py = y;
    it.pointer_moved = true;
    if (it.drag) {
        move_to(it.drag, x - it.drag_dx, y - it.drag_dy);
    } else if (it.press) {              /* the gadget drawn pressed while the pointer's on it */
        struct window *w = it.press;
        unsigned on = chrome_hit(&w->ch, x - w->icw.x, y - w->icw.y) == it.pressing ? it.pressing
                                                                                     : CH_NONE;

        if (on != w->ch.pressed) {
            w->ch.pressed = on;
            redraw(w);
        }
    } else if (it.active) {
        post(it.active, IDCMP_MOUSEMOVE, 0, ev->ie_Qualifier, 0, ev->ie_Time);
    }
}

/* The depth gadget: to the back, or to the front from the back. */
static void depth(struct window *w)
{
    if (it.screen.z[it.screen.nz - 1] == &w->icw)
        ic_raise(&it.screen, &w->icw);
    else
        ic_lower(&it.screen, &w->icw);
}

static void gadget_up(struct window *w)
{
    bool on = w->ch.pressed == it.pressing;

    w->ch.pressed = CH_NONE;
    redraw(w);
    it.press = nullptr;
    if (!on)
        return;
    if (it.pressing == CH_DEPTH) {
        depth(w);
    } else if (!w->close_by) {
        post(w, IDCMP_CLOSEWINDOW, 0, 0, 0, CurrentTime());
        w->close_by = CurrentTime() + CLOSE_DEADLINE;
        close_late();
    }
}

static void button(const struct InputEvent *ev)
{
    static const uint16_t down[3] = { SELECTDOWN, MENUDOWN, MIDDLEDOWN };
    static const uint16_t held[3] = { IEQ_LEFTBUTTON, IEQ_RBUTTON, IEQ_MIDBUTTON };
    unsigned b = ev->ie_Code;
    struct window *w;
    unsigned part;

    if (b > 2)
        return;
    if (ev->ie_Flags & IEF_UP) {
        it.buttons &= (uint16_t)~held[b];
        if (b == 0 && it.drag)
            it.drag = nullptr;
        if (b == 0 && it.press)
            gadget_up(it.press);
        if ((w = it.button_to[b])) {
            it.button_to[b] = nullptr;
            post(w, IDCMP_MOUSEBUTTONS, (uint16_t)(down[b] | 0x80), ev->ie_Qualifier, 0, ev->ie_Time);
        }
        return;
    }
    it.buttons |= held[b];
    w = of(ic_at(&it.screen, it.px, it.py));
    activate(w, ev->ie_Time);
    if (!w)
        return;
    part = chrome_hit(&w->ch, it.px - w->icw.x, it.py - w->icw.y);
    if (part != CH_DEPTH && it.screen.z[0] != &w->icw)
        ic_raise(&it.screen, &w->icw);
    if (part == CH_INSIDE) {
        it.button_to[b] = w;
        post(w, IDCMP_MOUSEBUTTONS, down[b], ev->ie_Qualifier, 0, ev->ie_Time);
    } else if (b == 0 && part == CH_TITLE && w->ch.flags & WFLG_DRAGBAR) {
        it.drag = w;
        it.drag_dx = it.px - w->icw.x;
        it.drag_dy = it.py - w->icw.y;
    } else if (b == 0 && (part == CH_DEPTH || part == CH_CLOSE)) {
        it.press = w;
        it.pressing = part;
        w->ch.pressed = part;
        redraw(w);
    }
}

static void key(const struct InputEvent *ev)
{
    struct window *w = it.active;
    uint16_t qual = ev->ie_Qualifier | (ev->ie_Flags & IEF_REPEAT ? IEQ_REPEAT : 0);
    bool up = ev->ie_Flags & IEF_UP;

    if (!w)
        return;
    if (!up && ev->ie_Char && w->idcmp & IDCMP_VANILLAKEY)
        post(w, IDCMP_VANILLAKEY, (uint16_t)ev->ie_Char, qual, ev->ie_Char, ev->ie_Time);
    else
        post(w, IDCMP_RAWKEY, (uint16_t)(ev->ie_Code | (up ? RAWKEY_UP : 0)), qual,
             up ? 0 : ev->ie_Char, ev->ie_Time);
}

/* What came on the ring since last time; an event overwritten while
 * being copied, or one more than the ring behind, is lost. */
static void input_events(void)
{
    uint64_t head;

    if (!it.ring)
        return;
    head = __atomic_load_n(&it.ring->ir_Head, __ATOMIC_ACQUIRE);
    if (head - it.itail > INPUT_RING)
        it.itail = head - INPUT_RING;
    while (it.itail < head) {
        struct InputEvent ev = it.ring->ir_Event[it.itail % INPUT_RING];

        if (__atomic_load_n(&it.ring->ir_Head, __ATOMIC_ACQUIRE) - it.itail > INPUT_RING) {
            it.itail = it.ring->ir_Head - INPUT_RING;
            continue;
        }
        it.itail++;
        switch (ev.ie_Class) {
        case IECLASS_MOVE:
            pointer(&ev);
            break;
        case IECLASS_BUTTON:
            button(&ev);
            break;
        case IECLASS_KEY:
            key(&ev);
            break;
        }
    }
}

/* --- Costs (v7-plan step 6): hwdb's argument on a board. --- */

constexpr uint64_t INTUITION_COSTS = 1;
constexpr unsigned COST_FRAMES = 60;

void drv_dma_clean(const void *p, uint64_t n);

/* COST_FRAMES frames of 's' composed into 'b[0]' and 'b[1]' in turn, 'w'
 * damaged whole or dragged 8 right and 4 down each: the ns a frame, and
 * the pixels. 'clean': each frame's damage, its bounds, cleaned after,
 * as a cached screen buffer needs before its commit. */
static uint64_t cost(struct ic_screen *s, struct BitMap *b, struct ic_window *w, bool drag, bool clean,
                     uint64_t *px)
{
    uint64_t t, p0;

    ic_damage(s, (struct GfxRect){ 0, 0, (int32_t)s->width - 1, (int32_t)s->height - 1 });
    ic_compose(s, &b[0]);
    ic_compose(s, &b[1]);
    t = CurrentTime();
    p0 = s->painted;
    for (unsigned f = 0; f < COST_FRAMES; f++) {
        struct GfxRect r = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
        struct ic_damage d[2] = { s->last, s->now };
        struct BitMap *to = &b[f & 1];

        if (drag)
            ic_move(s, w, w->x + 8, w->y + 4);
        else
            ic_damage_window(s, w, (struct GfxRect){ 0, 0, (int32_t)w->w - 1, (int32_t)w->h - 1 });
        d[1] = s->now;
        ic_compose(s, to);
        if (!clean)
            continue;
        for (unsigned k = 0; k < 2; k++)
            for (unsigned i = 0; i < d[k].n; i++) {
                r.MinX = d[k].r[i].MinX < r.MinX ? d[k].r[i].MinX : r.MinX;
                r.MinY = d[k].r[i].MinY < r.MinY ? d[k].r[i].MinY : r.MinY;
                r.MaxX = d[k].r[i].MaxX > r.MaxX ? d[k].r[i].MaxX : r.MaxX;
                r.MaxY = d[k].r[i].MaxY > r.MaxY ? d[k].r[i].MaxY : r.MaxY;
            }
        for (int32_t y = r.MinY; y <= r.MaxY; y++)
            drv_dma_clean((uint8_t *)to->bm_BaseAddr + (uint64_t)y * to->bm_BytesPerRow + (uint64_t)r.MinX * 4,
                          (uint64_t)(r.MaxX - r.MinX + 1) * 4);
    }
    *px = (s->painted - p0) / COST_FRAMES;
    return (CurrentTime() - t) / COST_FRAMES;
}

/* Composing's costs on display's NC screen buffers and on two cached
 * ones (decision 3): the whole screen, and a 640x480 window dragged,
 * each from a bitmap of the screen's size. Then the pointer alone,
 * which composes nothing: its commit, call to answer. Said, not checked. */
static void costs(void)
{
    static struct ic_screen s;
    struct ic_window full = { 0 }, small = { 0 };
    struct BitMap cached[2] = { 0 };
    struct BitMap *src;
    struct RastPort rp;
    uint32_t id[2] = { 0 };
    uint64_t c[4], px[4], commit = 0, t;
    bool ok = true;

    if (!(src = it.gfx->AllocBitMap(it.width, it.height, PIXFMT_XRGB8888, 0, nullptr))) {
        say("intuition: costs: no bitmap");
        return;
    }
    it.gfx->InitRastPort(&rp, src);
    for (uint32_t y = 0; y < it.height; y += 8) {
        rp.rp_FgPen = 0xff000000 | y * 0x10203u;
        it.gfx->RectFill(&rp, 0, (int32_t)y, (int32_t)it.width - 1, (int32_t)y + 7);
    }
    for (unsigned b = 0; b < 2; b++)
        ok &= alloc(it.width, it.height, DISPLAY_XRGB8888, DISPLAYA_CACHED, &id[b], &cached[b]) == 0;
    full = (struct ic_window){ .x = 0, .y = 0, .w = it.width, .h = it.height, .nparts = 1,
                               .part[0] = { { 0, 0, (int32_t)it.width - 1, (int32_t)it.height - 1 }, src, 0, 0 } };
    small = (struct ic_window){ .w = 640, .h = 480, .nparts = 1, .part[0] = { { 0, 0, 639, 479 }, src, 0, 0 } };
    for (unsigned k = 0; ok && k < 4; k++) {
        struct ic_window *w = k & 2 ? &small : &full;

        ic_init(&s, it.gfx, it.width, it.height);
        small.x = small.y = 0;
        ic_add(&s, w);
        c[k] = cost(&s, k & 1 ? cached : it.sbm, w, k & 2, k & 1, &px[k]);
    }
    for (unsigned b = 0; b < 2; b++)
        if (id[b]) {
            memset(it.dm, 0, sizeof *it.dm);
            it.dm->dm_Buffer = id[b];
            display(DISPLAY_FREE);
            ReleaseArea(cached[b].bm_BaseAddr);
        }
    it.gfx->FreeBitMap(src);
    for (unsigned i = 0; ok && i < 10; i++) {
        uint64_t seq;

        memset(it.dm, 0, sizeof *it.dm);
        it.dm->dm_Set[0] = (struct DisplayPlane){ 2, it.pointer, it.px + (int32_t)i * 4, it.py };
        it.dm->dm_Count = 1;
        t = CurrentTime();
        if (display(DISPLAY_COMMIT) != 0) {
            ok = false;
            break;
        }
        commit += CurrentTime() - t;
        for (seq = it.dm->dm_Seq; it.ds->ds_Disp < seq;)
            WaitAddr(&it.ds->ds_Disp, it.ds->ds_Disp, CurrentTime() + 100 * MS);
    }
    if (!ok) {
        say("intuition: costs: display wouldn't give cached buffers, or a commit");
        return;
    }
    KPrintF("intuition: costs, %lu frames each: the whole screen %lu us into NC, %lu us into cached and "
            "cleaned", (uint64_t)COST_FRAMES, c[0] / 1000, c[1] / 1000);
    KPrintF("intuition: costs: a 640x480 window dragged %lu us into NC, %lu us cached and cleaned (%lu "
            "pixels a frame); the pointer alone composes nothing, its commit %lu us",
            c[2] / 1000, c[3] / 1000, px[2], commit / 10 / 1000);
}

int main(uint64_t arg)
{
    struct Library *lib = IExec->OpenLibrary("gfx.library", 7);
    int64_t h;

    it.task = FindTask(nullptr);
    if (!lib || !(it.gfx = (const struct GfxIFace *)IExec->GetInterface(lib, "main", 1))) {
        say("intuition: no gfx.library");
        return 1;
    }
    if (CreateTask(watcher, (uint64_t)&latch_w) < 0 || CreateTask(watcher, (uint64_t)&input_w) < 0 ||
        CreateTask(awaiter, (uint64_t)&display_a) < 0 || CreateTask(awaiter, (uint64_t)&input_a) < 0) {
        say("intuition: no tasks");
        return 1;
    }
    if ((h = screen_up(CurrentTime() + 5000 * MS)) != 0) {
        say(h == ERR_NOSYS ? "intuition: display has no overlay and pointer planes to compose on"
                           : "intuition: no screen from display");
        return 2;
    }
    input_up(CurrentTime() + 1000 * MS);
    if (arg & INTUITION_COSTS)
        costs();
    if ((h = CreatePort("intuition", SIG_ACCEPT)) < 0) {
        say("intuition: \"intuition\" couldn't be published");
        return 1;
    }
    it.port = (Handle)h;
    KPrintF("intuition: the screen, %lux%lu, composed on display's plane 1, the pointer on 2%s",
            (uint64_t)it.width, (uint64_t)it.height, it.ring ? "; input's ring held" : "");
    for (;;) {
        uint64_t sigs;

        frame();
        sigs = WaitPorts(it.mp, it.nconns,
                         1ULL << SIG_ACCEPT | 1ULL << SIG_CONN | 1ULL << SIG_LATCH | 1ULL << SIG_DISPLAY |
                             1ULL << SIG_INPUT | 1ULL << SIG_INPUT_PORT | 1ULL << SIG_TIMER |
                             1ULL << SIG_DISPLAY_BACK | 1ULL << SIG_INPUT_BACK);
        if (it.ds && PortGone(&it.dp))
            display_lost();
        else if (!it.ds && sigs & 1ULL << SIG_DISPLAY_BACK)
            display_back();
        if (it.ring && PortGone(&it.ip))
            input_lost();
        else if (!it.ring && sigs & 1ULL << SIG_INPUT_BACK)
            input_up(0);
        for (struct MsgPort extra; ;) {
            struct MsgPort *mp = it.nconns < CONNS ? &it.mp[it.nconns] : &extra;

            if (AcceptPort(mp, it.port, SIG_CONN) != 0)
                break;
            if (mp == &extra)
                ClosePort(&extra);  /* full: told, not left waiting */
            else
                it.cl[it.nconns++] = (struct client){ 0 };
        }
        for (unsigned c = 0; c < it.nconns;) {
            struct IntuiMsg *m;

            while ((m = (struct IntuiMsg *)GetMsgSized(&it.mp[c], sizeof *m)))
                message(c, m);
            if (PortGone(&it.mp[c])) {
                gone(c);
                continue;
            }
            c++;
        }
        input_events();
        if (sigs & 1ULL << SIG_TIMER)
            close_late();
        flush_events();
        latch();
    }
}
