/*
 * servers/intuition/main.c -- intuition: windows composed on the screen
 * (v7-plan steps 3 and 4)
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
 * (decision 4): its test now, and in V8 the Workbench.
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

    /* display, and what intuition shows on it. */
    struct MsgPort dp;
    struct DisplayMsg *dm;
    const struct DisplaySync *ds;
    uint32_t width, height;
    uint32_t buf[2], pointer;
    struct BitMap sbm[2];
    unsigned back;
    int32_t px, py;
    bool flipping, pointer_moved;
    uint64_t flip_seq;

    /* input, and what's done with it. */
    struct MsgPort ip;
    const struct InputRing *ring;
    uint64_t itail;
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

static int64_t alloc(uint32_t w, uint32_t h, uint32_t format, uint32_t *id, struct BitMap *bm)
{
    int64_t e;

    memset(it.dm, 0, sizeof *it.dm);
    it.dm->dm_Width = w;
    it.dm->dm_Height = h;
    it.dm->dm_Format = format;
    if ((e = display(DISPLAY_ALLOC)) != 0)
        return e;
    *id = it.dm->dm_Buffer;
    return it.gfx->InitBitMap(bm, (void *)it.dm->dm_Address, w, h, it.dm->dm_Stride,
                              format == DISPLAY_XRGB8888 ? PIXFMT_XRGB8888 : PIXFMT_ARGB8888)
               ? 0
               : ERR_BADARG;
}

/* The second task: ds_Disp moved. */
static void watch(uint64_t)
{
    for (;;) {
        uint64_t v = it.ds->ds_Disp;

        Signal(it.task, 1ULL << SIG_LATCH);
        WaitAddr(&it.ds->ds_Disp, v, 0);
    }
}

static int64_t screen_up(void)
{
    struct BitMap pbm;
    int64_t e;

    if ((e = ConnectPortUntil(&it.dp, "display.0", SIG_DISPLAY, 2, 2 * sizeof *it.dm,
                              CurrentTime() + 5000 * MS)) != 0)
        return e;
    if (!(it.dm = AllocMsg(&it.dp, sizeof *it.dm)))
        return ERR_NOMEM;
    memset(it.dm, 0, sizeof *it.dm);
    if ((e = display(DISPLAY_INFO)) != 0)
        return e;
    it.ds = (const struct DisplaySync *)it.dm->dm_Sync;
    it.width = it.dm->dm_Width;
    it.height = it.dm->dm_Height;
    if (it.dm->dm_Count < 3 || !(it.dm->dm_Plane[1].di_Formats & 1u << DISPLAY_XRGB8888) ||
        it.dm->dm_Plane[1].di_MaxWidth < it.width || it.dm->dm_Plane[1].di_MaxHeight < it.height ||
        !(it.dm->dm_Plane[2].di_Formats & 1u << DISPLAY_ARGB8888))
        return ERR_NOSYS;
    for (unsigned b = 0; b < 2; b++)
        if ((e = alloc(it.width, it.height, DISPLAY_XRGB8888, &it.buf[b], &it.sbm[b])) != 0)
            return e;
    if ((e = alloc(POINTER_SIZE, POINTER_SIZE, DISPLAY_ARGB8888, &it.pointer, &pbm)) != 0)
        return e;
    for (uint32_t y = 0; y < POINTER_SIZE; y++)
        for (uint32_t x = 0; x < POINTER_SIZE; x++)
            ((uint32_t *)((uint8_t *)pbm.bm_BaseAddr + y * pbm.bm_BytesPerRow))[x] = pointer_pixel(x, y);
    it.px = (int32_t)it.width / 2;
    it.py = (int32_t)it.height / 2;
    ic_init(&it.screen, it.gfx, it.width, it.height);
    return CreateTask(watch, 0) < 0 ? ERR_NOMEM : 0;
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

static void frame(void)
{
    bool compose;
    uint64_t t;
    int64_t e;
    unsigned n = 0;

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
    if ((e = display(DISPLAY_COMMIT)) != 0) {
        say("intuition: display refused a frame");
        ExitProcess(e == ERR_PORT_DIED || e == ERR_TIMEOUT ? 3 : 4);
    }
    it.pointer_moved = false;
    it.flip_seq = it.dm->dm_Seq;
    it.flipping = true;
    if (compose)
        it.back ^= 1;
}

static void latch(void)
{
    if (it.flipping && it.ds->ds_Disp >= it.flip_seq) {
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

/* The third task: input's ring has more. */
static void watch_input(uint64_t)
{
    for (;;) {
        uint64_t v = it.ring->ir_Head;

        Signal(it.task, 1ULL << SIG_INPUT);
        WaitAddr(&it.ring->ir_Head, v, 0);
    }
}

/* input's ring, if nobody holds it: the Shell may (decision 4), and then
 * intuition goes on without it. */
static void input_up(void)
{
    const char *why = "intuition: no input: no pointer or keys";
    struct InputMsg *m;
    int64_t e;

    if (ConnectPortUntil(&it.ip, "input", SIG_INPUT_PORT, 2, 2 * sizeof *m, CurrentTime() + 1000 * MS)) {
        say(why);
        return;
    }
    if (!(m = AllocMsg(&it.ip, sizeof *m)))
        goto closed;
    memset(m, 0, sizeof *m);
    m->im_Msg.mn_Length = sizeof *m;
    m->im_Op = INPUT_OPEN;
    m->im_Task = it.task;
    if ((e = DoMsgUntil(&it.ip, &m->im_Msg, CurrentTime() + CALL_DEADLINE)) != 0 ||
        m->im_Msg.mn_Error != 0) {
        if (!e && m->im_Msg.mn_Error == ERR_BUSY)
            why = "intuition: input's ring is another's: no pointer or keys";
        goto closed;
    }
    it.ring = (const struct InputRing *)m->im_Ring;
    it.itail = it.ring->ir_Head;
    if (CreateTask(watch_input, 0) >= 0)
        return;
    it.ring = nullptr;
closed:
    ClosePort(&it.ip);
    say(why);
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

int main(uint64_t)
{
    struct Library *lib = IExec->OpenLibrary("gfx.library", 7);
    int64_t h;

    it.task = FindTask(nullptr);
    if (!lib || !(it.gfx = (const struct GfxIFace *)IExec->GetInterface(lib, "main", 1))) {
        say("intuition: no gfx.library");
        return 1;
    }
    if ((h = screen_up()) != 0) {
        say(h == ERR_NOSYS ? "intuition: display has no overlay and pointer planes to compose on"
                           : "intuition: no screen from display");
        return 2;
    }
    input_up();
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
        sigs = WaitPorts(it.mp, it.nconns, 1ULL << SIG_ACCEPT | 1ULL << SIG_CONN | 1ULL << SIG_LATCH |
                                               1ULL << SIG_INPUT | 1ULL << SIG_TIMER);
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
