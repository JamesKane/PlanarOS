/*
 * servers/intuition/main.c -- intuition: windows composed on the screen
 * (v7-plan steps 3 to 5; V8 step 4)
 *
 * planar/intuition.h has the protocol. intuition is display's client:
 * two full-screen XRGB buffers of display's, shown on plane 1 in turn
 * over the console (decision 2), and the pointer's sprite on plane 2.
 * A frame is composed (compose.c) into the buffer not on the screen once
 * the last flip has latched, so one is composed a vblank at most. A
 * second task waits on the sync page's ds_Disp and signals the first,
 * whose sleep is on its ports. The pointer moving is a commit of plane 2
 * alone, at a vblank at most too, with no window composed. A frame no
 * client waits for (the pointer, a drag, the chrome) is put off until
 * just before the vblank, so a window's frame presented meanwhile goes
 * with it (compose.h's ic_start).
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
 * V8 step 4: the screen bar (bar.c), pinned in front of every window,
 * with the clock from the RTC's driver, read once an hour and kept by
 * CurrentTime between. Menus: the active window's strip, kept here,
 * drawn and tracked here, so a hung client's still open; the right
 * button held, or a title clicked. A window resized by its size gadget
 * drags an outline, and at the release is resized at once, its old
 * buffer shown clipped (or the backdrop beyond it) until the client
 * gives buffers of the new size, taken at its next present. Requesters
 * are windows of intuition's own: a client's EasyRequest, answered when
 * chosen; the system menu's About (its Shell asks hwdb for a console
 * window: HWDB_NEW_SHELL, V8 step 5); and a window whose close gadget gets
 * no CloseWindow within the deadline, which asks whether to close it,
 * and on Close asks hwdb to end its client (HWDB_KILL_CLIENT).
 *
 * V8 step 7: intuition's settings, GetPrefs and SetPrefs: the pointer's
 * size, its own (two sprites, either shown), and the HUD and the
 * backdrop's colours, the Workbench's, which each window asking for
 * IDCMP_NEWPREFS is told have changed. The saved ones are read at start
 * (intuition.prefs, planar/prefs.h): at once if dos is there, else by a
 * task of its own once it is. A WFLG_BACKDROP window is kept behind the
 * others, and a click doesn't raise it.
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
#include <planar/drv.h>
#include <planar/intuition.h>
#include <planar/prefs.h>
#include <planar/syscalls.h>

#include "bar.h"
#include "chrome.h"
#include "compose.h"
#include "events.h"
#include "log.h"
#include "pointer.h"

constexpr int64_t SIG_ACCEPT = 16, SIG_CONN = 17, SIG_LATCH = 18, SIG_DISPLAY = 19;
constexpr int64_t SIG_INPUT = 20, SIG_INPUT_PORT = 21, SIG_TIMER = 22;
constexpr int64_t SIG_DISPLAY_BACK = 23, SIG_INPUT_BACK = 24, SIG_CALL = 25, SIG_PREFS = 26;
constexpr int64_t SIG_AWAIT = 16;               /* an awaiting task's own, for its connection */
constexpr unsigned CONNS = 16;
constexpr uint64_t MS = 1000000;
constexpr uint64_t CALL_DEADLINE = 1000 * MS;   /* for display's and input's answers (V6 step 7) */
constexpr uint64_t CLOSE_DEADLINE = 1000 * MS;  /* for CloseWindow, after the close gadget */
constexpr uint64_t COMMIT_LEAD = 4 * MS;        /* a frame nobody waits for: committed this long before the vblank */
constexpr uint64_t COMPOSE_FIXED = MS / 5;      /* a frame's cost beyond its pixels' */
constexpr uint64_t RATE_PIXELS = 65536;         /* a frame this big or more says what a pixel costs */
constexpr unsigned RATES = 8;
constexpr uint64_t SECOND = 1000 * MS;
constexpr uint64_t RTC_AGAIN = 3600 * SECOND;   /* the RTC read again after this */
constexpr unsigned REQS = 4;                    /* requesters on the screen, at most */
constexpr uint32_t OUTLINE = PEN_TEXT_LIGHT;    /* a resize's outline */
constexpr uint32_t BEYOND = IC_BACKDROP;        /* a resized window's inside past its buffer */

/* What an ic_window on the screen is (its kind). */
enum : uint32_t { K_WINDOW, K_BAR, K_MENU, K_OUTLINE, K_REQUEST };

/* Why a requester is up. */
enum : uint8_t { R_CLIENT, R_HUNG, R_ABOUT };

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
    uint32_t min_w, min_h, max_w, max_h;    /* a resize's limits, the inside's */
    struct BitMap next[INTUI_BUFFERS];      /* ChangeWindowBuffers', taken at its next present */
    uint64_t next_area[INTUI_BUFFERS];
    uint32_t next_buffers;                  /* 0: none waiting */
    bool has_strip;
    struct IntuiMenuStrip strip;
};

/* A requester: a window of intuition's own. */
struct req {
    bool used;
    uint8_t why;                    /* R_* */
    uint8_t conn;                   /* R_CLIENT's connection */
    struct IntuiMsg *m;             /* R_CLIENT's call, answered when chosen */
    struct window *of;              /* R_HUNG's window */
    struct ic_window icw;
    struct chrome ch;
    struct BitMap *top, *side, *body;
    struct request q;
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
    uint32_t buf[2], pointer[2];    /* the pointer's sprites: 1x and 2x */
    struct BitMap sbm[2], pbm[2];
    unsigned back;
    int32_t px, py;
    bool flipping, pointer_moved;
    bool cached;                    /* the screen buffers: DISPLAYI_CACHED, cleaned each frame */
    uint64_t flip_seq;
    uint64_t period;                /* the vblanks', from display's refresh */
    uint64_t frame_due;             /* a frame nobody waits for, to be started then; 0 none */
    uint64_t rate[RATES];           /* the last big frames' picoseconds a pixel */
    unsigned nrate;
    uint64_t close_next;            /* close_late's next deadline; 0 none */

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

    /* V8 step 4: the type, the screen bar, menus, resizing and requesters. */
    struct TextFont *sans, *bold;
    struct chrome_fonts fonts;
    struct ic_window bar_w;
    struct BitMap *bar_bm;
    struct bar bar;
    struct IntuiMenuStrip sys;      /* the system menu */
    int menu_title;                 /* the title open: the bar's numbering; -1 none */
    bool menu_held;                 /* by the right button: chosen at its release */
    struct ic_window menu_w;
    struct BitMap *menu_bm;
    struct menu menu;
    struct window *sizing;          /* by its size gadget: to size_w by size_h, its inside */
    int32_t size_dx, size_dy;       /* the pointer from its bottom right corner */
    uint32_t size_w, size_h;
    struct ic_window outline[4];
    bool outlined;
    struct req req[REQS];
    struct req *req_press;          /* a requester's button held down */
    int req_button;
    uint64_t rtc_secs, rtc_at;      /* the RTC's seconds, read at CurrentTime's rtc_at; 0 none */
    uint64_t clock_next;            /* the clock drawn again then */
    uint64_t clock_secs;            /* the minute it shows, for INFO */

    uint64_t frames, nanos;
    uint64_t move_at, flip_move_at;     /* the oldest move not yet committed, and in the flip */
    uint64_t moves, move_nanos, move_max, read_max;
    uint64_t last_move, gaps[4];        /* input's ie_Time of the last move read, and the gaps since */

    /* V8 step 7: the settings. */
    struct IntuiPrefs prefs;
    bool prefs_set;                     /* by a client: the saved ones aren't taken */
} it = { .prefs = { 0, INTUI_POINTER_AUTO, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM } };

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
    for (unsigned b = 0; b < 2; b++)
        if (it.pbm[b].bm_BaseAddr)
            ReleaseArea(it.pbm[b].bm_BaseAddr);
    if (it.ds)
        ReleaseArea(it.ds);
    ClosePort(&it.dp);
    it.sbm[0] = it.sbm[1] = it.pbm[0] = it.pbm[1] = (struct BitMap){ 0 };
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
    it.period = it.dm->dm_RefreshMilliHz ? 1000000000000ull / it.dm->dm_RefreshMilliHz : 16667 * 1000;
    if (it.dm->dm_Count < 3 || !(it.dm->dm_Plane[1].di_Formats & 1u << DISPLAY_XRGB8888) ||
        it.dm->dm_Plane[1].di_MaxWidth < it.width || it.dm->dm_Plane[1].di_MaxHeight < it.height ||
        !(it.dm->dm_Plane[2].di_Formats & 1u << DISPLAY_ARGB8888))
        return ERR_NOSYS;
    it.cached = it.dm->dm_Flags & DISPLAYI_CACHED;     /* decision 3: whichever display says is faster */
    for (unsigned b = 0; b < 2; b++)
        if ((e = alloc(it.width, it.height, DISPLAY_XRGB8888, it.cached ? DISPLAYA_CACHED : 0, &it.buf[b],
                       &it.sbm[b])) != 0)
            return e;
    /* Both sprites, 1x and 2x: the setting picks (V8 step 7). */
    for (uint32_t ps = 1; ps <= 2; ps++) {
        struct BitMap *pbm = &it.pbm[ps - 1];

        if ((e = alloc(POINTER_SIZE * ps, POINTER_SIZE * ps, DISPLAY_ARGB8888, 0, &it.pointer[ps - 1], pbm)) != 0)
            return e;
        for (uint32_t y = 0; y < POINTER_SIZE * ps; y++)
            for (uint32_t x = 0; x < POINTER_SIZE * ps; x++)
                ((uint32_t *)((uint8_t *)pbm->bm_BaseAddr + y * pbm->bm_BytesPerRow))[x] = pointer_pixel(x / ps, y / ps);
    }
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
    KLog("intuition: display went: the screen composed again when it's back");
    screen_down();
    await(&display_a);
}

static void display_back(void)
{
    int64_t e = screen_up(0);

    if (e == 0) {
        KLog("intuition: display is back: the screen composed again");
        return;
    }
    screen_down();
    if (e == ERR_BADARG || e == ERR_NOSYS) {
        KLog("intuition: display came back with another screen");
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

void drv_dma_clean(const void *p, uint64_t n);

/* What a frame composed into 'to' (its damage 'd', the last frame's and
 * its own, as ic_compose repainted them), cleaned to memory: a cached
 * screen buffer's, before its commit. */
static void clean(const struct ic_damage d[2], const struct BitMap *to)
{
    for (unsigned k = 0; k < 2; k++)
        for (unsigned i = 0; i < d[k].n; i++) {
            struct GfxRect r = d[k].r[i];
            uint64_t w = (uint64_t)(r.MaxX - r.MinX + 1) * 4;

            /* Rows the rectangle spans whole are one range: one clean, its barriers once. */
            if (w == to->bm_BytesPerRow) {
                drv_dma_clean((uint8_t *)to->bm_BaseAddr + (uint64_t)r.MinY * w, (uint64_t)(r.MaxY - r.MinY + 1) * w);
                continue;
            }
            for (int32_t y = r.MinY; y <= r.MaxY; y++)
                drv_dma_clean((uint8_t *)to->bm_BaseAddr + (uint64_t)y * to->bm_BytesPerRow +
                                  (uint64_t)r.MinX * 4, w);
        }
}

/* What composing the next frame should take: its pixels at the worst
 * rate of the last big frames. None yet (the first frame, the whole
 * screen): a period, so it's composed at once. */
static uint64_t compose_cost(void)
{
    uint64_t rate = 0;

    if (!it.nrate)
        return it.period;
    for (unsigned i = 0; i < RATES; i++)
        rate = it.rate[i] > rate ? it.rate[i] : rate;
    return COMPOSE_FIXED + ic_pending(&it.screen) * rate / 1000;
}

/* intuition's one timer, at whichever is first. */
static void rearm(void)
{
    uint64_t t = it.close_next;

    if (it.frame_due && (!t || it.frame_due < t))
        t = it.frame_due;
    if (it.clock_next && (!t || it.clock_next < t))
        t = it.clock_next;
    ArmTimer(t, SIG_TIMER);
}

/* The pointer's sprite, as the setting says: by the screen's size, or either. */
static uint32_t sprite(void)
{
    uint32_t ps = it.prefs.ip_PointerSize ? it.prefs.ip_PointerSize : pointer_scale(it.width, it.height);

    return it.pointer[ps - 1];
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
    /* Not waited for: put off until just before the vblank (ic_start). */
    t = ic_start(CurrentTime(), it.ds->ds_Time, it.period, COMMIT_LEAD, compose ? compose_cost() : 0,
                 unsafe());
    if (t) {
        it.frame_due = t;
        rearm();
        return;
    }
    it.frame_due = 0;
    memset(it.dm, 0, sizeof *it.dm);
    if (compose) {
        uint64_t px = it.screen.painted;
        struct ic_damage d[2] = { it.screen.last, it.screen.now };

        t = CurrentTime();
        ic_compose(&it.screen, &it.sbm[it.back]);
        if (it.cached)
            clean(d, &it.sbm[it.back]);
        t = CurrentTime() - t;
        px = it.screen.painted - px;
        if (px >= RATE_PIXELS)
            it.rate[it.nrate++ % RATES] = t * 1000 / px;
        it.nanos += t;
        it.frames++;
        taken();
        it.dm->dm_Set[n++] = (struct DisplayPlane){ 1, it.buf[it.back], 0, 0 };
    }
    it.dm->dm_Set[n++] = (struct DisplayPlane){ 2, sprite(), it.px, it.py };
    it.dm->dm_Count = n;
    if ((e = display(DISPLAY_COMMIT)) == ERR_PORT_DIED) {
        display_lost();
        return;
    }
    if (e != 0) {
        KLog("intuition: display refused a frame");
        ExitProcess(e == ERR_TIMEOUT ? 3 : 4);
    }
    it.pointer_moved = false;
    it.flip_move_at = it.move_at;
    it.move_at = 0;
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
        if (it.flip_move_at && it.ds->ds_Time > it.flip_move_at) {
            uint64_t age = it.ds->ds_Time - it.flip_move_at;

            it.moves++;
            it.move_nanos += age;
            it.move_max = age > it.move_max ? age : it.move_max;
        }
        it.flip_move_at = 0;
    }
}

/* --- Windows. --- */

static struct window *of(struct ic_window *icw)
{
    return icw && icw->kind == K_WINDOW ? (struct window *)((char *)icw - offsetof(struct window, icw))
                                        : nullptr;
}

static struct req *req_of(struct ic_window *icw)
{
    return icw && icw->kind == K_REQUEST ? (struct req *)((char *)icw - offsetof(struct req, icw)) : nullptr;
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

/* Let go of the Area at 'area' unless a window's buffer (or one it's to
 * take) or a client's IntuiAck lies in it. */
static void release(uint64_t area, const struct window *except)
{
    if (!area)
        return;
    for (unsigned i = 0; i < IC_WINDOWS; i++) {
        const struct window *w = &it.win[i];

        if (w != except && w->used)
            for (unsigned b = 0; b < INTUI_BUFFERS; b++)
                if ((b < w->buffers && w->area[b] == area) || (b < w->next_buffers && w->next_area[b] == area))
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

static void post_at(struct window *w, uint32_t cls, uint16_t code, uint16_t qual, uint32_t ch,
                    uint64_t time, int32_t x, int32_t y)
{
    struct client *cl = &it.cl[w->conn];
    struct IntuiEvent e = {
        .iev_Time = time, .iev_Class = cls, .iev_Window = id_of(w), .iev_Code = code,
        .iev_Qualifier = (uint16_t)(qual | it.buttons), .iev_Char = ch, .iev_MouseX = x, .iev_MouseY = y,
    };

    if (w->idcmp & cls && !w->removed && cl->page)
        ie_push(&cl->q, &e, &cl->page->ip_Lost);
}

/* An event at the pointer, in the window's inside's coordinates. */
static void post(struct window *w, uint32_t cls, uint16_t code, uint16_t qual, uint32_t ch,
                 uint64_t time)
{
    post_at(w, cls, code, qual, ch, time, it.px - (w->icw.x + w->ix), it.py - (w->icw.y + w->iy));
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

/* Its parts: the inside from its buffer, as much of it as the buffer
 * holds and BEYOND past that; then the chrome's. */
static void layout(struct window *w)
{
    const struct BitMap *bm = &w->bm[w->cur];
    int32_t bw = (int32_t)(bm->bm_Width < w->iw ? bm->bm_Width : w->iw);
    int32_t bh = (int32_t)(bm->bm_Height < w->ih ? bm->bm_Height : w->ih);
    int32_t x1 = w->ix + (int32_t)w->iw - 1, y1 = w->iy + (int32_t)w->ih - 1;
    unsigned n;

    w->icw.part[0] = (struct ic_part){ { w->ix, w->iy, w->ix + bw - 1, w->iy + bh - 1 }, bm, 0, 0, 0 };
    chrome_parts(&w->ch, w->top, w->side, &w->icw);
    n = w->icw.nparts;
    if (bw < (int32_t)w->iw)
        w->icw.part[n++] = (struct ic_part){ { w->ix + bw, w->iy, x1, y1 }, nullptr, 0, 0, BEYOND };
    if (bh < (int32_t)w->ih)
        w->icw.part[n++] = (struct ic_part){ { w->ix, w->iy + bh, w->ix + bw - 1, y1 }, nullptr, 0, 0, BEYOND };
    w->icw.nparts = n;
}

static void menu_close(void);
static void bar_redraw(void);
static void req_free(struct req *r);

static void activate(struct window *w, uint64_t time)
{
    struct window *was = it.active;

    if (was == w)
        return;
    it.active = w;
    if (it.menu_title > 0)              /* a menu of the strip that was */
        menu_close();
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
    bar_redraw();
}

static void move_to(struct window *w, int32_t x, int32_t y)
{
    if (x == w->icw.x && y == w->icw.y)
        return;
    ic_move(&it.screen, &w->icw, x, y);
    line_of(w)->is_LeftEdge = x;
    line_of(w)->is_TopEdge = y;
}

static void outline_off(void)
{
    if (it.outlined)
        for (unsigned k = 0; k < 4; k++)
            ic_remove(&it.screen, &it.outline[k]);
    it.outlined = false;
}

/* Nothing of intuition's points at 'w' any more. */
static void forget(struct window *w)
{
    if (it.active == w) {
        if (it.menu_title > 0)
            menu_close();
        it.active = nullptr;
        bar_redraw();
    }
    if (it.drag == w)
        it.drag = nullptr;
    if (it.press == w)
        it.press = nullptr;
    if (it.sizing == w) {
        outline_off();
        it.sizing = nullptr;
    }
    for (unsigned b = 0; b < 3; b++)
        if (it.button_to[b] == w)
            it.button_to[b] = nullptr;
    for (unsigned r = 0; r < REQS; r++)
        if (it.req[r].used && it.req[r].of == w)
            req_free(&it.req[r]);
}

/* Off the screen, its buffers and its chrome let go. */
static void take_off(struct window *w)
{
    uint64_t area[2 * INTUI_BUFFERS];
    unsigned n = 0;

    forget(w);
    if (w->shown)
        ic_remove(&it.screen, &w->icw);
    w->shown = false;
    for (unsigned b = 0; b < INTUI_BUFFERS; b++) {
        area[n++] = w->area[b];
        area[n++] = w->next_area[b];
        w->area[b] = w->next_area[b] = 0;
    }
    w->buffers = w->next_buffers = 0;
    for (unsigned i = 0; i < n; i++)
        release(area[i], w);
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

static bool ask_hung(struct window *w);

static void close_late(void)
{
    uint64_t now = CurrentTime(), next = 0;

    for (unsigned i = 0; i < IC_WINDOWS; i++) {
        struct window *w = &it.win[i];

        if (!w->used || !w->close_by)
            continue;
        if (w->close_by <= now) {
            w->close_by = 0;
            if (!ask_hung(w))
                remove_late(w);
        } else if (!next || w->close_by < next) {
            next = w->close_by;
        }
    }
    it.close_next = next;
    rearm();
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
        (!(n.top = it.gfx->AllocBitMap(n.ch.w, INTUI_TITLEBAR + chrome_bottom(q->im_Flags),
                                       PIXFMT_XRGB8888, 0, nullptr)) ||
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
    n.min_w = q->im_MinWidth ? q->im_MinWidth : 1;
    n.min_h = q->im_MinHeight ? q->im_MinHeight : 1;
    n.max_w = q->im_MaxWidth ? q->im_MaxWidth : GFX_MAX_SIZE;
    n.max_h = q->im_MaxHeight ? q->im_MaxHeight : GFX_MAX_SIZE;
    n.icw = (struct ic_window){ q->im_Left, q->im_Top, n.ch.w, n.ch.h, 0, { 0 }, K_WINDOW };
    memcpy(n.title, q->im_Title, INTUI_TITLE);
    n.title[INTUI_TITLE - 1] = 0;
    *w = n;
    w->ch.title = w->title;
    w->ch.fonts = &it.fonts;
    layout(w);
    w->icw.nparts = 0;                  /* nothing shown until its first frame */
    if (w->top)
        chrome_draw(it.gfx, w->top, w->side, &w->ch);
    cl->lines |= 1u << line;
    *line_of(w) = (struct IntuiSync){ .is_LeftEdge = q->im_Left, .is_TopEdge = q->im_Top,
                                      .is_Width = n.iw, .is_Height = n.ih };
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

/* The buffers ChangeWindowBuffers gave, taken: the old ones let go of
 * unless they're among the new. */
static void rebuffered(struct window *w)
{
    uint64_t old[INTUI_BUFFERS];
    unsigned had = w->buffers;

    for (unsigned b = 0; b < INTUI_BUFFERS; b++) {
        old[b] = w->area[b];
        w->bm[b] = w->next[b];
        w->area[b] = w->next_area[b];
        w->next_area[b] = 0;
    }
    w->buffers = w->next_buffers;
    w->next_buffers = 0;
    for (unsigned b = 0; b < had; b++)
        release(old[b], nullptr);
}

static int64_t present(unsigned c, const struct IntuiMsg *q)
{
    struct window *w = find(c, q->im_Window);
    const struct BitMap *bm;
    bool whole;

    if (!w || w->removed)
        return ERR_NOTFOUND;
    if (q->im_Index >= (w->next_buffers ? w->next_buffers : w->buffers) || q->im_Count > INTUI_RECTS ||
        q->im_Frame <= w->frame)
        return ERR_BADARG;
    bm = w->next_buffers ? &w->next[q->im_Index] : &w->bm[q->im_Index];
    for (unsigned i = 0; i < q->im_Count; i++) {
        const struct GfxRect *r = &q->im_Rect[i];

        if (r->MinX < 0 || r->MinY < 0 || r->MinX > r->MaxX || r->MinY > r->MaxY ||
            r->MaxX >= (int64_t)bm->bm_Width || r->MaxY >= (int64_t)bm->bm_Height)
            return ERR_BADARG;
    }
    if ((whole = w->next_buffers))
        rebuffered(w);
    w->cur = q->im_Index;
    w->frame = q->im_Frame;
    layout(w);
    if (!w->shown) {
        if (!(w->shown = w->ch.flags & WFLG_BACKDROP ? ic_add_back(&it.screen, &w->icw)
                                                      : ic_add(&it.screen, &w->icw))) {
            w->icw.nparts = 0;
            return ERR_FULL;
        }
        if (w->ch.flags & WFLG_ACTIVATE)
            activate(w, CurrentTime());
        return 0;
    }
    if (!q->im_Count || whole)
        ic_damage_window(&it.screen, &w->icw,
                         (struct GfxRect){ w->ix, w->iy, w->ix + (int32_t)w->iw - 1, w->iy + (int32_t)w->ih - 1 });
    for (unsigned i = 0; i < q->im_Count; i++)
        ic_damage_window(&it.screen, &w->icw,
                         (struct GfxRect){ w->ix + q->im_Rect[i].MinX, w->iy + q->im_Rect[i].MinY,
                                           w->ix + q->im_Rect[i].MaxX, w->iy + q->im_Rect[i].MaxY });
    return 0;
}

/* --- Resizing (V8 step 4). --- */

/* 'w' made iw by ih inside, at once: its chrome drawn again at the new
 * size, its buffer shown as far as it goes, and the client told. */
static bool resize(struct window *w, uint32_t iw, uint32_t ih)
{
    struct chrome ch = w->ch;
    struct BitMap *top = nullptr, *side = nullptr;
    int32_t ix, iy;

    if (iw == w->iw && ih == w->ih)
        return true;
    chrome_size(w->ch.flags, iw, ih, &ch, &ix, &iy);
    if (!(w->ch.flags & WFLG_BORDERLESS) &&
        (!(top = it.gfx->AllocBitMap(ch.w, INTUI_TITLEBAR + chrome_bottom(ch.flags), PIXFMT_XRGB8888, 0,
                                     nullptr)) ||
         !(side = it.gfx->AllocBitMap(2 * INTUI_BORDER, ih, PIXFMT_XRGB8888, 0, nullptr)))) {
        it.gfx->FreeBitMap(top);
        return false;
    }
    if (w->top) {
        it.gfx->FreeBitMap(w->top);
        it.gfx->FreeBitMap(w->side);
    }
    w->top = top;
    w->side = side;
    w->ch = ch;
    w->iw = iw;
    w->ih = ih;
    if (w->top)
        chrome_draw(it.gfx, w->top, w->side, &w->ch);
    ic_resize(&it.screen, &w->icw, ch.w, ch.h);
    layout(w);
    line_of(w)->is_Width = iw;
    line_of(w)->is_Height = ih;
    post_at(w, IDCMP_NEWSIZE, 0, 0, 0, CurrentTime(), (int32_t)iw, (int32_t)ih);
    return true;
}

/* The outline of 'it.sizing' at size_w by size_h: four thin windows of
 * a flat colour, pinned. */
static void outline_set(void)
{
    const struct window *w = it.sizing;
    int32_t ow = (int32_t)(it.size_w + w->ch.w - w->iw), oh = (int32_t)(it.size_h + w->ch.h - w->ih);
    const struct GfxRect e[4] = { { 0, 0, ow - 1, 1 }, { 0, oh - 2, ow - 1, oh - 1 }, { 0, 2, 1, oh - 3 },
                                  { ow - 2, 2, ow - 1, oh - 3 } };

    outline_off();
    for (unsigned k = 0; k < 4; k++) {
        struct ic_window *o = &it.outline[k];
        uint32_t ew = (uint32_t)(e[k].MaxX - e[k].MinX + 1), eh = (uint32_t)(e[k].MaxY - e[k].MinY + 1);

        *o = (struct ic_window){ w->icw.x + e[k].MinX, w->icw.y + e[k].MinY, ew, eh, 1, { 0 }, K_OUTLINE };
        o->part[0] = (struct ic_part){ { 0, 0, (int32_t)ew - 1, (int32_t)eh - 1 }, nullptr, 0, 0, OUTLINE };
        ic_pin(&it.screen, o);
    }
    it.outlined = true;
}

static uint32_t limit(int64_t v, uint32_t lo, uint32_t hi)
{
    return v < (int64_t)lo ? lo : v > (int64_t)hi ? hi : (uint32_t)v;
}

/* The inside's size with the size gadget at the pointer, within the
 * window's limits and the screen. */
static void size_track(void)
{
    const struct window *w = it.sizing;
    int64_t cw = (int64_t)w->ch.w - w->iw, chh = (int64_t)w->ch.h - w->ih;
    uint32_t mw = (uint32_t)((int64_t)it.width - w->icw.x - cw), mh = (uint32_t)((int64_t)it.height - w->icw.y - chh);

    mw = mw > w->max_w ? w->max_w : mw;
    mh = mh > w->max_h ? w->max_h : mh;
    it.size_w = limit((int64_t)it.px + it.size_dx - w->icw.x - cw, w->min_w, mw > w->min_w ? mw : w->min_w);
    it.size_h = limit((int64_t)it.py + it.size_dy - w->icw.y - chh, w->min_h, mh > w->min_h ? mh : w->min_h);
    outline_set();
}

static void size_start(struct window *w)
{
    it.sizing = w;
    it.size_dx = w->icw.x + (int32_t)w->ch.w - it.px;
    it.size_dy = w->icw.y + (int32_t)w->ch.h - it.py;
    size_track();
}

static void size_end(void)
{
    struct window *w = it.sizing;

    outline_off();
    it.sizing = nullptr;
    if (!resize(w, it.size_w, it.size_h))
        KLog("intuition: no memory for a resized window's chrome: left as it was");
}

/* --- The screen bar and its clock (V8 step 4). --- */

static void copy(char *to, const char *from, unsigned n)
{
    unsigned i = 0;

    for (; from[i] && i < n - 1; i++)
        to[i] = from[i];
    to[i] = 0;
}

static void bar_redraw(void)
{
    if (!it.bar_bm)
        return;
    it.bar.strip = it.active && it.active->has_strip ? &it.active->strip : nullptr;
    it.bar.open = it.menu_title;
    bar_layout(it.gfx, &it.bar);
    bar_draw(it.gfx, it.bar_bm, &it.bar);
    ic_damage_window(&it.screen, &it.bar_w, it.bar_w.part[0].r);
}

/* The RTC's seconds, from its driver if it's there: connected to, read
 * and let go, so intuition holds nothing of it between. */
static bool rtc_read(uint64_t *secs)
{
    struct MsgPort mp;
    struct DrvRequest *r;
    bool ok = false;

    if (ConnectPort(&mp, "rtc.0", SIG_CALL, 2, 2 * sizeof *r) != 0)
        return false;
    if ((r = AllocMsg(&mp, sizeof *r))) {
        memset(r, 0, sizeof *r);
        r->dr_Msg.mn_Length = sizeof *r;
        r->dr_Op = DRV_READ;
        r->dr_Len = 8;
        if (DoMsgUntil(&mp, &r->dr_Msg, CurrentTime() + 100 * MS) == 0 && r->dr_Msg.mn_Error == 0 &&
            r->dr_Len == 8) {
            *secs = 0;
            for (unsigned i = 0; i < 8; i++)
                *secs |= (uint64_t)r->dr_Data[i] << (8 * i);
            ok = *secs != 0;
        }
    }
    ClosePort(&mp);
    return ok;
}

/* The time of day drawn, and again at the next minute; with no RTC yet,
 * nothing, and it's looked for again in 5 s. */
static void clock_tick(void)
{
    uint64_t now = CurrentTime(), s;
    char was[sizeof it.bar.clock];

    if ((!it.rtc_at || now - it.rtc_at > RTC_AGAIN) && rtc_read(&s)) {
        it.rtc_secs = s;
        it.rtc_at = now;
    }
    memcpy(was, it.bar.clock, sizeof was);
    if (!it.rtc_at) {
        it.bar.clock[0] = 0;
        it.clock_next = now + 5 * SECOND;
    } else {
        s = it.rtc_secs + (now - it.rtc_at) / SECOND;
        bar_clock(it.bar.clock, s);
        it.clock_secs = s - s % 60;
        it.clock_next = now + (60 - s % 60) * SECOND;
    }
    if (memcmp(was, it.bar.clock, sizeof was) != 0)
        bar_redraw();
    rearm();
}

/* The system menu: About, and Shell (a console window, hwdb's: V8 step
 * 5); Reboot and Shutdown wait for power to have them. */
static void system_menu(void)
{
    static const char *const items[] = { "About Planar", nullptr, "Shell", nullptr, "Reboot", "Shutdown" };
    struct IntuiMenuStrip *s = &it.sys;

    copy(s->ms_Title[0], "PlanarOS", INTUI_LABEL);
    s->ms_Menus = 1;
    for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++) {
        struct IntuiMenuItem *mi = &s->ms_Item[s->ms_Items++];

        if (!items[i]) {
            mi->mi_Flags = ITEMBAR;
            continue;
        }
        copy(mi->mi_Label, items[i], INTUI_LABEL);
        mi->mi_Flags = i > 2 ? NM_ITEMDISABLED : 0;
    }
}

/* System Sans, the bar pinned at the top, and the clock. Without the
 * fonts, the 8x8; without the bar's bitmap, no bar. */
static void bar_up(void)
{
    struct TextAttr sans = { "System Sans", 13, 0, 0 }, bold = { "System Sans", 13, FSF_BOLD, 0 };

    it.sans = it.gfx->OpenTextFont(&sans);
    it.bold = it.gfx->OpenTextFont(&bold);
    if (!it.sans || !it.bold)
        KLog("intuition: System Sans wouldn't open: the chrome in the 8x8 font");
    it.fonts = (struct chrome_fonts){ it.sans, it.bold };
    it.menu_title = -1;
    system_menu();
    if (!(it.bar_bm = it.gfx->AllocBitMap(it.width, INTUI_SCREENBAR, PIXFMT_XRGB8888, 0, nullptr))) {
        KLog("intuition: no memory for the screen bar");
        return;
    }
    it.bar = (struct bar){ .width = it.width, .fonts = &it.fonts, .open = -1 };
    it.bar_w = (struct ic_window){ 0, 0, it.width, INTUI_SCREENBAR, 1, { 0 }, K_BAR };
    it.bar_w.part[0] = (struct ic_part){ { 0, 0, (int32_t)it.width - 1, (int32_t)INTUI_SCREENBAR - 1 },
                                         it.bar_bm, 0, 0, 0 };
    ic_pin(&it.screen, &it.bar_w);
    clock_tick();
    bar_redraw();
}

/* --- Menus (V8 step 4). --- */

/* The strip title 'title' of the bar opens: the system's, or the
 * active window's. */
static const struct IntuiMenuStrip *strip_of(int title)
{
    if (title == 0)
        return &it.sys;
    return it.active && it.active->has_strip && title - 1 < (int)it.active->strip.ms_Menus ? &it.active->strip
                                                                                         : nullptr;
}

static void menu_close(void)
{
    if (it.menu_bm) {
        ic_remove(&it.screen, &it.menu_w);
        it.gfx->FreeBitMap(it.menu_bm);
        it.menu_bm = nullptr;
    }
    if (it.menu_title >= 0) {
        it.menu_title = -1;
        bar_redraw();
    }
}

/* Title 'title' open, its menu under it unless it's disabled. */
static void menu_open(int title)
{
    const struct IntuiMenuStrip *s = strip_of(title);
    int32_t x;

    if (title == it.menu_title)
        return;
    menu_close();
    if (!s)
        return;
    it.menu_title = title;
    bar_redraw();
    if (title > 0 && s->ms_Disabled & 1u << (title - 1))
        return;
    it.menu = (struct menu){ .strip = s, .menu = title ? (unsigned)title - 1 : 0, .hot = -1 };
    menu_layout(it.gfx, &it.fonts, &it.menu);
    if (!it.menu.count ||
        !(it.menu_bm = it.gfx->AllocBitMap(it.menu.w, it.menu.h, PIXFMT_XRGB8888, 0, nullptr)))
        return;
    menu_draw(it.gfx, &it.fonts, it.menu_bm, &it.menu);
    x = it.bar.x0[title];
    if (x + (int32_t)it.menu.w > (int32_t)it.width)
        x = (int32_t)it.width - (int32_t)it.menu.w;
    it.menu_w = (struct ic_window){ x < 0 ? 0 : x, (int32_t)INTUI_SCREENBAR, it.menu.w, it.menu.h, 1, { 0 }, K_MENU };
    it.menu_w.part[0] = (struct ic_part){ { 0, 0, (int32_t)it.menu.w - 1, (int32_t)it.menu.h - 1 }, it.menu_bm, 0, 0, 0 };
    ic_pin(&it.screen, &it.menu_w);
}

/* The pointer at (x, y) with the menus up: a title it's on opens, an
 * item it's on is highlighted. */
static void menu_track(int32_t x, int32_t y)
{
    int title = bar_hit(&it.bar, x, y), hot = -1;

    if (title >= 0)
        menu_open(title);
    if (!it.menu_bm)
        return;
    hot = menu_hit(&it.menu, x - it.menu_w.x, y - it.menu_w.y);
    if (hot != it.menu.hot) {
        it.menu.hot = hot;
        menu_draw(it.gfx, &it.fonts, it.menu_bm, &it.menu);
        ic_damage_window(&it.screen, &it.menu_w, it.menu_w.part[0].r);
    }
}

/* Item 'index' of w's strip (its 'item' of menu 'menu') chosen: a
 * CHECKIT item checked or not, and the client told. */
static void pick(struct window *w, unsigned index, unsigned menu, unsigned item, uint64_t time)
{
    struct IntuiMenuItem *mi = &w->strip.ms_Item[index];

    if (mi->mi_Flags & CHECKIT)
        mi->mi_Flags ^= CHECKED;
    post(w, IDCMP_MENUPICK, FULLMENUNUM(menu, item, NOSUB), 0,
         mi->mi_Flags & CHECKIT && mi->mi_Flags & CHECKED ? 1 : 0, time);
}

static void about(void);
static void new_shell(void);

/* The menus closed, and the highlighted item chosen if it can be. */
static void menu_choose(uint64_t time)
{
    struct menu m = it.menu;
    bool open = it.menu_bm, system = m.strip == &it.sys;
    struct window *w = it.active;

    menu_close();
    if (!open || !menu_choosable(&m, m.hot))
        return;
    if (system) {
        if (m.hot == 0)
            about();
        else if (m.hot == 2)
            new_shell();
        return;
    }
    if (w && m.strip == &w->strip)
        pick(w, m.first + (unsigned)m.hot, m.menu, (unsigned)m.hot, time);
}

/* A key with the GUI key held: an item of the active window's strip it
 * chooses, if any. True if it was such a key, chosen or not. */
static bool menu_key(const struct InputEvent *ev)
{
    struct window *w = it.active;
    uint32_t c = ev->ie_Char;
    unsigned item = 0;

    if (!w || !w->has_strip || ev->ie_Flags & IEF_UP || !(ev->ie_Qualifier & (IEQ_LGUI | IEQ_RGUI)) || !c ||
        c > 0x7e)
        return false;
    c = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
    for (unsigned i = 0; i < w->strip.ms_Items; i++) {
        const struct IntuiMenuItem *mi = &w->strip.ms_Item[i];

        item = i && mi->mi_Menu == w->strip.ms_Item[i - 1].mi_Menu ? item + 1 : 0;
        if ((uint32_t)(uint8_t)mi->mi_Key != c)
            continue;
        if (!(w->strip.ms_Disabled & 1u << mi->mi_Menu) && !(mi->mi_Flags & (ITEMBAR | NM_ITEMDISABLED)))
            pick(w, i, mi->mi_Menu, item, ev->ie_Time);
        return true;
    }
    return false;
}

/* SetMenuStrip's strip, copied from the client's page once and checked
 * there: labels ended, items in their menus' order. */
static int64_t menus(unsigned c, const struct IntuiMsg *q)
{
    struct window *w = find(c, q->im_Window);
    struct IntuiMenuStrip *s;
    unsigned in_menu = 0;

    if (!w || w->removed)
        return ERR_NOTFOUND;
    if (!it.cl[c].ack)
        return ERR_BADARG;
    if (it.menu_title > 0 && w == it.active)
        menu_close();
    s = &w->strip;
    memcpy(s, &it.cl[c].ack->ia_Strip, sizeof *s);
    w->has_strip = false;
    if (s->ms_Menus > INTUI_MENU_TITLES || s->ms_Items > INTUI_MENU_ITEMS)
        return ERR_BADARG;
    for (unsigned i = 0; i < INTUI_MENU_TITLES; i++)
        s->ms_Title[i][INTUI_LABEL - 1] = 0;
    for (unsigned i = 0; i < s->ms_Items; i++) {
        struct IntuiMenuItem *mi = &s->ms_Item[i];

        mi->mi_Label[INTUI_LABEL - 1] = 0;
        in_menu = i && mi->mi_Menu == s->ms_Item[i - 1].mi_Menu ? in_menu + 1 : 0;
        if (mi->mi_Menu >= s->ms_Menus || (i && mi->mi_Menu < s->ms_Item[i - 1].mi_Menu) || in_menu > 62)
            return ERR_BADARG;
    }
    w->has_strip = s->ms_Menus > 0;
    if (w == it.active)
        bar_redraw();
    return 0;
}

/* ChangeWindowBuffers': checked now, taken at the window's next present. */
static int64_t rebuffer(unsigned c, const struct IntuiMsg *q)
{
    struct window *w = find(c, q->im_Window);
    struct BitMap bm[INTUI_BUFFERS] = { 0 };
    uint64_t area[INTUI_BUFFERS] = { 0 }, old[INTUI_BUFFERS];
    unsigned had;
    int64_t e = 0;

    if (!w || w->removed)
        return ERR_NOTFOUND;
    if (q->im_Buffers < 1 || q->im_Buffers > INTUI_BUFFERS || q->im_Width != w->iw || q->im_Height != w->ih)
        return ERR_BADARG;
    for (unsigned b = 0; b < q->im_Buffers && !e; b++)
        e = buffer(&q->im_Buffer[b], w->iw, w->ih, &bm[b], &area[b]);
    if (e) {
        for (unsigned b = 0; b < INTUI_BUFFERS; b++)
            release(area[b], nullptr);
        return e;
    }
    had = w->next_buffers;              /* a change not yet taken: dropped */
    for (unsigned b = 0; b < INTUI_BUFFERS; b++) {
        old[b] = w->next_area[b];
        w->next[b] = bm[b];
        w->next_area[b] = area[b];
    }
    w->next_buffers = q->im_Buffers;
    for (unsigned b = 0; b < had; b++)
        release(old[b], nullptr);
    return 0;
}

/* --- Requesters (V8 step 4). --- */

static struct req *req_new(void)
{
    for (unsigned i = 0; i < REQS; i++)
        if (!it.req[i].used) {
            it.req[i] = (struct req){ 0 };
            return &it.req[i];
        }
    return nullptr;
}

static void req_free(struct req *r)
{
    if (!r->used)
        return;
    ic_remove(&it.screen, &r->icw);
    it.gfx->FreeBitMap(r->top);
    it.gfx->FreeBitMap(r->side);
    it.gfx->FreeBitMap(r->body);
    if (it.req_press == r)
        it.req_press = nullptr;
    *r = (struct req){ 0 };
}

/* r's text laid out and drawn in a window of intuition's, at the front,
 * centred over 'over' or the screen, below the bar. */
static bool req_show(struct req *r, const struct window *over)
{
    int32_t ix, iy, x, y, cx, cy;

    if (!request_layout(it.gfx, &it.fonts, &r->q, it.width > 64 ? it.width - 40 : it.width))
        return false;
    chrome_size(0, r->q.w, r->q.h, &r->ch, &ix, &iy);
    r->ch.title = r->q.r.ir_Title;
    r->ch.active = true;
    r->ch.fonts = &it.fonts;
    if (!(r->top = it.gfx->AllocBitMap(r->ch.w, INTUI_TITLEBAR + INTUI_BORDER, PIXFMT_XRGB8888, 0, nullptr)) ||
        !(r->side = it.gfx->AllocBitMap(2 * INTUI_BORDER, r->q.h, PIXFMT_XRGB8888, 0, nullptr)) ||
        !(r->body = it.gfx->AllocBitMap(r->q.w, r->q.h, PIXFMT_XRGB8888, 0, nullptr))) {
        it.gfx->FreeBitMap(r->top);
        it.gfx->FreeBitMap(r->side);
        return false;
    }
    chrome_draw(it.gfx, r->top, r->side, &r->ch);
    request_draw(it.gfx, &it.fonts, r->body, &r->q);
    cx = over && over->shown ? over->icw.x + (int32_t)over->icw.w / 2 : (int32_t)it.width / 2;
    cy = over && over->shown ? over->icw.y + (int32_t)over->icw.h / 2 : (int32_t)it.height / 2;
    x = cx - (int32_t)r->ch.w / 2;
    y = cy - (int32_t)r->ch.h / 2;
    x = x + (int32_t)r->ch.w > (int32_t)it.width ? (int32_t)it.width - (int32_t)r->ch.w : x;
    y = y + (int32_t)r->ch.h > (int32_t)it.height ? (int32_t)it.height - (int32_t)r->ch.h : y;
    r->icw = (struct ic_window){ x < 0 ? 0 : x, y < (int32_t)INTUI_SCREENBAR ? (int32_t)INTUI_SCREENBAR : y,
                                 r->ch.w, r->ch.h, 0, { 0 }, K_REQUEST };
    r->icw.part[0] = (struct ic_part){ { ix, iy, ix + (int32_t)r->q.w - 1, iy + (int32_t)r->q.h - 1 }, r->body, 0, 0, 0 };
    chrome_parts(&r->ch, r->top, r->side, &r->icw);
    if (!ic_add(&it.screen, &r->icw)) {
        r->used = true;
        req_free(r);
        return false;
    }
    r->used = true;
    return true;
}

static bool kill_client(unsigned c);

/* r answered: its caller told, or what intuition asked done. */
static void req_answer(struct req *r, int answer)
{
    struct window *w = r->of;
    struct IntuiMsg *m = r->m;
    uint8_t why = r->why, conn = r->conn;

    req_free(r);
    if (why == R_CLIENT) {
        m->im_Index = (uint32_t)answer;
        m->im_Msg.mn_Error = 0;
        ReplyMsgLater(&it.mp[conn], &m->im_Msg);
    } else if (why == R_HUNG && w && w->used) {
        if (answer == 0) {              /* Wait: asked again after another deadline */
            w->close_by = CurrentTime() + CLOSE_DEADLINE;
            close_late();
        } else if (!kill_client(w->conn)) {
            remove_late(w);
        }
    }
}

/* The front requester's inside button at the pointer, or -1. */
static int req_button_at(const struct req *r)
{
    return request_hit(&r->q, it.px - r->icw.x - r->icw.part[0].r.MinX, it.py - r->icw.y - r->icw.part[0].r.MinY);
}

static void req_pressed(struct req *r, int button)
{
    if (r->q.pressed == button)
        return;
    r->q.pressed = button;
    request_draw(it.gfx, &it.fonts, r->body, &r->q);
    ic_damage_window(&it.screen, &r->icw, r->icw.part[0].r);
}

static void about(void)
{
    struct req *r = req_new();

    if (!r)
        return;
    r->why = R_ABOUT;
    copy(r->q.r.ir_Title, "About Planar", INTUI_TITLE);
    copy(r->q.r.ir_Body, "Planar OS, version 8\nThe desktop: intuition 8.4, its type in System Sans\n"
                         "Windows composed on the CPU, the pointer on a plane of its own", INTUI_REQ_BODY);
    copy(r->q.r.ir_Gadgets, "OK", INTUI_REQ_GADGETS);
    req_show(r, nullptr);
}

/* w's close gadget went unanswered (decision 6, V8 step 4): asked
 * whether to close it. False if no requester could be shown. */
static bool ask_hung(struct window *w)
{
    struct req *r = req_new();

    if (!r)
        return false;
    r->why = R_HUNG;
    r->of = w;
    copy(r->q.r.ir_Title, "Program not answering", INTUI_TITLE);
    copy(r->q.r.ir_Body, "The program isn't answering.\nClose it?", INTUI_REQ_BODY);
    copy(r->q.r.ir_Gadgets, "Close|Wait", INTUI_REQ_GADGETS);
    if (!req_show(r, w))
        return false;
    KPrintF("intuition: window %lu wasn't closed within %lu ms of its close gadget: asked whether to "
            "close it", (uint64_t)id_of(w), CLOSE_DEADLINE / MS);
    return true;
}

/* EasyRequest's: copied from the client's page once; answered when chosen. */
static int64_t requested(unsigned c, struct IntuiMsg *m, const struct IntuiMsg *q, bool *held)
{
    struct window *w = q->im_Window ? find(c, q->im_Window) : nullptr;
    struct req *r;

    if (q->im_Window && (!w || w->removed))
        return ERR_NOTFOUND;
    if (!it.cl[c].ack)
        return ERR_BADARG;
    if (!(r = req_new()))
        return ERR_FULL;
    memcpy(&r->q.r, &it.cl[c].ack->ia_Request, sizeof r->q.r);
    r->why = R_CLIENT;
    r->conn = (uint8_t)c;
    r->m = m;
    if (!req_show(r, w))
        return ERR_NOMEM;
    *held = true;
    return 0;
}

/* A request of hwdb's, on a connection made for it: 0 or why not. */
static int64_t hwdb_call(uint32_t op, uint64_t conn)
{
    static struct HwdbMsg m;        /* 4 KiB: not on the stack; the main task's alone */

    memset(&m, 0, sizeof m);
    m.hm_Msg.mn_Length = sizeof m;
    m.hm_Op = op;
    m.hm_Conn = conn;
    return CallPort("hwdb", SIG_CALL, &m.hm_Msg, CurrentTime() + CALL_DEADLINE);
}

/* The system menu's Shell: hwdb starts a console window (V8 step 5). */
static void new_shell(void)
{
    int64_t e = hwdb_call(HWDB_NEW_SHELL, 0);

    if (e != 0)
        KPrintF("intuition: hwdb wouldn't start a Shell in a window (%ld)", e);
}

/* hwdb asked to end client c, which isn't answering (V8 step 4): true
 * if it did. */
static bool kill_client(unsigned c)
{
    int64_t e = hwdb_call(HWDB_KILL_CLIENT, (uint64_t)it.mp[c].mp_Conn);

    if (e == 0)
        KLog("intuition: a client not answering, ended by hwdb as asked");
    else
        KPrintF("intuition: hwdb wouldn't end a client not answering (%ld)", e);
    return e == 0;
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

/* --- Settings (V8 step 7). --- */

static bool prefs_ok(const struct IntuiPrefs *p)
{
    return p->ip_Hud <= 1 && p->ip_PointerSize <= INTUI_POINTER_LARGE && p->ip_BackdropTop <= 0xffffff &&
           p->ip_BackdropBottom <= 0xffffff;
}

/* New settings in force: the pointer's sprite, and every window that
 * asked told. */
static void prefs_take(const struct IntuiPrefs *p)
{
    it.prefs = *p;
    it.pointer_moved = true;
    for (unsigned i = 0; i < IC_WINDOWS; i++)
        if (it.win[i].used && !it.win[i].removed && it.win[i].idcmp & IDCMP_NEWPREFS)
            post(&it.win[i], IDCMP_NEWPREFS, 0, 0, 0, CurrentTime());
}

/* The saved ones, each tag held to its range: true if there were any. */
static bool prefs_read(struct Dos *dos, struct IntuiPrefs *p)
{
    struct TagItem t[PREFS_TAGS + 1];
    int64_t n = ReadPrefs(dos, "intuition", t, PREFS_TAGS + 1);

    for (int64_t i = 0; i < n; i++) {
        struct IntuiPrefs q = *p;
        uint32_t v = (uint32_t)t[i].ti_Data;

        if (t[i].ti_Tag == PREFS_Hud)
            q.ip_Hud = v;
        else if (t[i].ti_Tag == PREFS_PointerSize)
            q.ip_PointerSize = v;
        else if (t[i].ti_Tag == PREFS_BackdropTop)
            q.ip_BackdropTop = v;
        else if (t[i].ti_Tag == PREFS_BackdropBottom)
            q.ip_BackdropBottom = v;
        if (prefs_ok(&q))
            *p = q;
    }
    return n > 0;
}

static struct {
    struct IntuiPrefs prefs;
    volatile uint64_t done;             /* 2: read, to be taken */
} saved;

/* dos wasn't there at start: waited for by a task of its own. */
static void loader(uint64_t)
{
    struct Dos dos;
    struct IntuiPrefs p = it.prefs;
    bool any = false;

    if (OpenDosUntil(&dos, SIG_AWAIT, DEADLINE_NONE) == 0) {
        any = prefs_read(&dos, &p);
        CloseDos(&dos);
    }
    saved.prefs = p;
    __atomic_store_n(&saved.done, any ? 2 : 1, __ATOMIC_RELEASE);
    Signal(it.task, 1ULL << SIG_PREFS);
}

static void prefs_up(void)
{
    struct Dos dos;

    if (OpenDosUntil(&dos, SIG_CALL, 0) == 0) {
        if (prefs_read(&dos, &it.prefs))
            KLog("intuition: the saved settings taken");
        CloseDos(&dos);
    } else if (CreateTask(loader, 0) < 0) {
        KLog("intuition: no task to read the saved settings: the defaults kept");
    }
}

static void prefs_loaded(void)
{
    if (__atomic_load_n(&saved.done, __ATOMIC_ACQUIRE) != 2)
        return;
    saved.done = 1;
    if (it.prefs_set)
        return;
    prefs_take(&saved.prefs);
    KLog("intuition: the saved settings taken");
}

/* INFO's places on the screen, for tests that click them. */
static void places(struct IntuiMsg *m)
{
    for (unsigned i = 0; i < it.bar.n && i < 1 + INTUI_MENU_TITLES; i++)
        m->im_TitleX[i] = (it.bar.x0[i] + it.bar.x1[i]) / 2;
    if (it.menu_bm) {
        m->im_MenuRect = (struct GfxRect){ it.menu_w.x, it.menu_w.y, it.menu_w.x + (int32_t)it.menu_w.w - 1,
                                           it.menu_w.y + (int32_t)it.menu_w.h - 1 };
        for (unsigned i = 0; i < it.menu.count && i < 16; i++)
            m->im_ItemY[i] = it.menu_w.y + (it.menu.y[i] + it.menu.y[i + 1]) / 2;
    }
    for (unsigned z = 0; z < it.screen.nz; z++) {
        struct req *r = req_of(it.screen.z[z]);

        if (!r)
            continue;
        for (unsigned i = 0; i < r->q.nbuttons && i < 4; i++) {
            struct GfxRect b = r->q.button[i];
            int32_t dx = r->icw.x + r->icw.part[0].r.MinX, dy = r->icw.y + r->icw.part[0].r.MinY;

            m->im_ReqButton[i] = (struct GfxRect){ b.MinX + dx, b.MinY + dy, b.MaxX + dx, b.MaxY + dy };
        }
        break;
    }
}

static void message(unsigned c, struct IntuiMsg *m)
{
    static struct IntuiMsg q;
    struct window *w;
    int64_t e = 0;
    bool held = false;

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
        m->im_Windows = 0;              /* clients': not the bar, a menu or a requester */
        for (unsigned z = 0; z < it.screen.nz; z++)
            m->im_Windows += it.screen.z[z]->kind == K_WINDOW;
        m->im_Input = it.ring != nullptr;
        m->im_InputOpens = it.input_opens;
        m->im_Moves = it.moves;
        m->im_MoveNanos = it.move_nanos;
        m->im_MoveMax = it.move_max;
        m->im_ReadMax = it.read_max;
        memcpy(m->im_MoveGaps, it.gaps, sizeof it.gaps);
        m->im_Menu = (uint32_t)(it.menu_title + 1);
        m->im_Requests = 0;
        for (unsigned r = 0; r < REQS; r++)
            m->im_Requests += it.req[r].used;
        m->im_Clock = it.clock_secs;
        memset(m->im_TitleX, 0, sizeof m->im_TitleX);
        memset(&m->im_MenuRect, 0, sizeof m->im_MenuRect);
        memset(m->im_ItemY, 0, sizeof m->im_ItemY);
        memset(m->im_ReqButton, 0, sizeof m->im_ReqButton);
        places(m);
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
    case INTUI_MENUS:
        e = menus(c, &q);
        break;
    case INTUI_REBUFFER:
        e = rebuffer(c, &q);
        break;
    case INTUI_REQUEST:
        e = requested(c, m, &q, &held);
        break;
    case INTUI_GETPREFS:
        m->im_Prefs = it.prefs;
        break;
    case INTUI_SETPREFS:
        if (!prefs_ok(&q.im_Prefs)) {
            e = ERR_BADARG;
            break;
        }
        it.prefs_set = true;
        prefs_take(&q.im_Prefs);
        break;
    default:
        e = ERR_BADARG;
    }
    if (held)                           /* answered when chosen */
        return;
    m->im_Msg.mn_Error = (int32_t)e;
    ReplyMsgLater(&it.mp[c], &m->im_Msg);
}

/* Connection c went: its windows and its page go, and the last
 * connection takes its place. */
static void gone(unsigned c)
{
    unsigned last = it.nconns - 1;
    uint64_t ack = it.cl[c].ack_area;

    for (unsigned r = 0; r < REQS; r++)
        if (it.req[r].used && it.req[r].why == R_CLIENT && it.req[r].conn == c)
            req_free(&it.req[r]);       /* its caller's gone: nobody to answer */
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
        for (unsigned r = 0; r < REQS; r++)
            if (it.req[r].used && it.req[r].why == R_CLIENT && it.req[r].conn == last)
                it.req[r].conn = (uint8_t)c;
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
            KLog("intuition: input is back: its ring opened again, the pointer where it was");
        return;
    }
    if (e == ERR_BUSY) {
        KLog("intuition: input's ring is another's: no pointer or keys");
        return;
    }
    if (it.input_opens == 0 && deadline)
        KLog("intuition: no input: no pointer or keys until it comes");
    await(&input_a);
}

/* input went: nothing is held down any more. */
static void input_lost(void)
{
    KLog("intuition: input went: its ring opened again when it's back");
    watch_set(&input_w, nullptr);
    ReleaseArea(it.ring);
    ClosePort(&it.ip);
    it.ring = nullptr;
    it.buttons = 0;
    it.drag = nullptr;
    for (unsigned b = 0; b < 3; b++)
        it.button_to[b] = nullptr;
    it.menu_held = false;
    menu_close();
    if (it.sizing) {
        outline_off();
        it.sizing = nullptr;
    }
    if (it.req_press) {
        req_pressed(it.req_press, -1);
        it.req_press = nullptr;
    }
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
    if (!it.move_at)
        it.move_at = ev->ie_Time;
    if (it.menu_title >= 0 || it.menu_held) {
        menu_track(x, y);
    } else if (it.sizing) {
        size_track();
    } else if (it.req_press) {
        req_pressed(it.req_press, req_button_at(it.req_press) == it.req_button ? it.req_button : -1);
    } else if (it.drag) {
        /* Not under the screen bar: its title bar stays below it. */
        int32_t top = y - it.drag_dy;

        move_to(it.drag, x - it.drag_dx, top < (int32_t)INTUI_SCREENBAR ? (int32_t)INTUI_SCREENBAR : top);
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
    if (ic_back(&it.screen) == &w->icw)
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

/* A button let go over the menus, the size gadget or a requester's
 * button: true if one of those had it. */
static bool button_up_ours(unsigned b, uint64_t time)
{
    struct req *r = it.req_press;

    if (b == 1 && it.menu_held) {
        it.menu_held = false;
        menu_choose(time);
        return true;
    }
    if (b == 0 && it.menu_bm && it.menu.hot >= 0) {
        menu_choose(time);
        return true;
    }
    if (b == 0 && it.sizing) {
        size_end();
        return true;
    }
    if (b == 0 && r) {
        bool on = req_button_at(r) == it.req_button;

        it.req_press = nullptr;
        req_pressed(r, -1);
        if (on)
            req_answer(r, request_answer(&r->q, it.req_button));
        return true;
    }
    return false;
}

/* A button pressed over the bar, a menu or a requester, or with the
 * menus up: true if it was theirs. The right button over the bar, or
 * over anything while the active window has a strip, holds the menus. */
static bool button_down_ours(unsigned b, struct ic_window *at)
{
    struct req *r = req_of(at);
    int title;

    if (at && at->kind == K_BAR) {
        title = bar_hit(&it.bar, it.px, it.py);
        if (b == 1) {
            it.menu_held = true;
            if (title >= 0)
                menu_open(title);
        } else if (b == 0) {
            if (title >= 0 && title != it.menu_title)
                menu_open(title);
            else
                menu_close();
        }
        return true;
    }
    if (it.menu_title >= 0 || it.menu_held) {
        if (at && at->kind == K_MENU) {
            it.menu_held |= b == 1;
            return true;
        }
        if (!it.menu_held)
            menu_close();
        return true;
    }
    if (b == 1 && it.active && it.active->has_strip && !(r)) {
        it.menu_held = true;
        return true;
    }
    if (r) {
        ic_raise(&it.screen, &r->icw);
        if (b == 0 && (it.req_button = req_button_at(r)) >= 0) {
            it.req_press = r;
            req_pressed(r, it.req_button);
        }
        return true;
    }
    return at && at->kind == K_OUTLINE;
}

static void button(const struct InputEvent *ev)
{
    static const uint16_t down[3] = { SELECTDOWN, MENUDOWN, MIDDLEDOWN };
    static const uint16_t held[3] = { IEQ_LEFTBUTTON, IEQ_RBUTTON, IEQ_MIDBUTTON };
    unsigned b = ev->ie_Code;
    struct ic_window *at;
    struct window *w;
    unsigned part;

    if (b > 2)
        return;
    if (ev->ie_Flags & IEF_UP) {
        it.buttons &= (uint16_t)~held[b];
        if (button_up_ours(b, ev->ie_Time))
            return;
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
    at = ic_at(&it.screen, it.px, it.py);
    if (button_down_ours(b, at))
        return;
    w = of(at);
    activate(w, ev->ie_Time);
    if (!w)
        return;
    part = chrome_hit(&w->ch, it.px - w->icw.x, it.py - w->icw.y);
    if (part != CH_DEPTH && !(w->ch.flags & WFLG_BACKDROP) && ic_front(&it.screen) != &w->icw)
        ic_raise(&it.screen, &w->icw);
    if (part == CH_INSIDE) {
        it.button_to[b] = w;
        post(w, IDCMP_MOUSEBUTTONS, down[b], ev->ie_Qualifier, 0, ev->ie_Time);
    } else if (b == 0 && part == CH_TITLE && w->ch.flags & WFLG_DRAGBAR) {
        it.drag = w;
        it.drag_dx = it.px - w->icw.x;
        it.drag_dy = it.py - w->icw.y;
    } else if (b == 0 && part == CH_SIZE) {
        size_start(w);
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

    if (!w || menu_key(ev))
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
            if (CurrentTime() - ev.ie_Time > it.read_max)
                it.read_max = CurrentTime() - ev.ie_Time;
            if (it.last_move && ev.ie_Time >= it.last_move) {
                uint64_t g = ev.ie_Time - it.last_move;

                if (g < 1000 * MS)
                    it.gaps[g < 1 * MS ? 0 : g < 20 * MS ? 1 : g < 100 * MS ? 2 : 3]++;
            }
            it.last_move = ev.ie_Time;
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

/* COST_FRAMES frames of 's' composed into 'b[0]' and 'b[1]' in turn, 'w'
 * damaged whole or dragged 8 right and 4 down each: the ns a frame, and
 * the pixels. 'cached': each frame's damage cleaned after, as frame()
 * does for a cached screen buffer. */
static uint64_t cost(struct ic_screen *s, struct BitMap *b, struct ic_window *w, bool drag, bool cached,
                     uint64_t *px)
{
    uint64_t t, p0;

    ic_damage(s, (struct GfxRect){ 0, 0, (int32_t)s->width - 1, (int32_t)s->height - 1 });
    ic_compose(s, &b[0]);
    ic_compose(s, &b[1]);
    t = CurrentTime();
    p0 = s->painted;
    for (unsigned f = 0; f < COST_FRAMES; f++) {
        struct BitMap *to = &b[f & 1];
        struct ic_damage d[2];

        if (drag)
            ic_move(s, w, w->x + 8, w->y + 4);
        else
            ic_damage_window(s, w, (struct GfxRect){ 0, 0, (int32_t)w->w - 1, (int32_t)w->h - 1 });
        d[0] = s->last;
        d[1] = s->now;
        ic_compose(s, to);
        if (cached)
            clean(d, to);
    }
    *px = (s->painted - p0) / COST_FRAMES;
    return (CurrentTime() - t) / COST_FRAMES;
}

/* Composing's costs into NC screen buffers and into cached ones
 * (decision 3), intuition's own and two of the other kind: the whole
 * screen, and a 640x480 window dragged,
 * each from a bitmap of the screen's size. Then the pointer alone,
 * which composes nothing: its commit, call to answer. Said, not checked. */
static void costs(void)
{
    static struct ic_screen s;
    struct ic_window full = { 0 }, small = { 0 };
    struct BitMap other[2] = { 0 };
    struct BitMap *nc = it.cached ? other : it.sbm, *cached = it.cached ? it.sbm : other;
    struct BitMap *src;
    struct RastPort rp;
    uint32_t id[2] = { 0 };
    uint64_t c[4], px[4], commit = 0, t;
    bool ok = true;

    if (!(src = it.gfx->AllocBitMap(it.width, it.height, PIXFMT_XRGB8888, 0, nullptr))) {
        KLog("intuition: costs: no bitmap");
        return;
    }
    it.gfx->InitRastPort(&rp, src);
    for (uint32_t y = 0; y < it.height; y += 8) {
        rp.rp_FgPen = 0xff000000 | y * 0x10203u;
        it.gfx->RectFill(&rp, 0, (int32_t)y, (int32_t)it.width - 1, (int32_t)y + 7);
    }
    for (unsigned b = 0; b < 2; b++)
        ok &= alloc(it.width, it.height, DISPLAY_XRGB8888, it.cached ? 0 : DISPLAYA_CACHED, &id[b],
                    &other[b]) == 0;
    full = (struct ic_window){ .x = 0, .y = 0, .w = it.width, .h = it.height, .nparts = 1,
                               .part[0] = { { 0, 0, (int32_t)it.width - 1, (int32_t)it.height - 1 }, src, 0, 0, 0 } };
    small = (struct ic_window){ .w = 640, .h = 480, .nparts = 1, .part[0] = { { 0, 0, 639, 479 }, src, 0, 0, 0 } };
    for (unsigned k = 0; ok && k < 4; k++) {
        struct ic_window *w = k & 2 ? &small : &full;

        ic_init(&s, it.gfx, it.width, it.height);
        small.x = small.y = 0;
        ic_add(&s, w);
        c[k] = cost(&s, k & 1 ? cached : nc, w, k & 2, k & 1, &px[k]);
    }
    for (unsigned b = 0; b < 2; b++)
        if (id[b]) {
            memset(it.dm, 0, sizeof *it.dm);
            it.dm->dm_Buffer = id[b];
            display(DISPLAY_FREE);
            ReleaseArea(other[b].bm_BaseAddr);
        }
    it.gfx->FreeBitMap(src);
    for (unsigned i = 0; ok && i < 10; i++) {
        uint64_t seq;

        memset(it.dm, 0, sizeof *it.dm);
        it.dm->dm_Set[0] = (struct DisplayPlane){ 2, sprite(), it.px + (int32_t)i * 4, it.py };
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
        KLog("intuition: costs: display wouldn't give the other kind of buffer, or a commit");
        return;
    }
    KPrintF("intuition: costs, %lu frames each: the whole screen %lu us into NC, %lu us into cached and "
            "cleaned", (uint64_t)COST_FRAMES, c[0] / 1000, c[1] / 1000);
    KPrintF("intuition: costs: a 640x480 window dragged %lu us into NC, %lu us into cached and cleaned, "
            "%lu pixels a frame", c[2] / 1000, c[3] / 1000, px[2]);
    KPrintF("intuition: costs: the pointer alone composes nothing; its commit %lu us", commit / 10 / 1000);
}

int main(uint64_t arg)
{
    struct Library *lib = IExec->OpenLibrary("gfx.library", 7);
    int64_t h;

    it.task = FindTask(nullptr);
    if (!lib || !(it.gfx = (const struct GfxIFace *)IExec->GetInterface(lib, "main", 1))) {
        KLog("intuition: no gfx.library");
        return 1;
    }
    if (CreateTask(watcher, (uint64_t)&latch_w) < 0 || CreateTask(watcher, (uint64_t)&input_w) < 0 ||
        CreateTask(awaiter, (uint64_t)&display_a) < 0 || CreateTask(awaiter, (uint64_t)&input_a) < 0) {
        KLog("intuition: no tasks");
        return 1;
    }
    if ((h = screen_up(CurrentTime() + 5000 * MS)) != 0) {
        KLog(h == ERR_NOSYS ? "intuition: display has no overlay and pointer planes to compose on"
                           : "intuition: no screen from display");
        return 2;
    }
    prefs_up();
    bar_up();
    input_up(CurrentTime() + 1000 * MS);
    if (arg & INTUITION_COSTS)
        costs();
    if ((h = CreatePort("intuition", SIG_ACCEPT)) < 0) {
        KLog("intuition: \"intuition\" couldn't be published");
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
                             1ULL << SIG_DISPLAY_BACK | 1ULL << SIG_INPUT_BACK | 1ULL << SIG_PREFS);
        if (sigs & 1ULL << SIG_PREFS)
            prefs_loaded();
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
        if (sigs & 1ULL << SIG_TIMER) {
            close_late();
            if (it.clock_next && CurrentTime() >= it.clock_next)
                clock_tick();
        }
        flush_events();
        latch();
    }
}
