/*
 * servers/intuition/main.c -- intuition: windows composed on the screen
 * (v7-plan step 3)
 *
 * planar/intuition.h has the protocol. intuition is display's client:
 * two full-screen XRGB buffers of display's, shown on plane 1 in turn
 * over the console (decision 2), and the pointer's sprite on plane 2.
 * A frame is composed (compose.c) into the buffer not on the screen once
 * the last flip has latched, so one is composed a vblank at most. A
 * second task waits on the sync page's ds_Disp and signals the first,
 * whose sleep is on its ports.
 *
 * A window's buffers are its client's, shared with intuition read-only.
 * intuition describes each one itself (InitBitMap) from what OPEN said,
 * checked against the Area it lies in, so a client's BitMap is never
 * read. A window joins the screen, at the front, at its first frame.
 * Composing a frame takes each window's latest: its is_Safe moves then,
 * and its is_Disp when the flip latches. A frame that changed nothing on
 * the screen (a window off it) is safe and shown at once.
 *
 * When a client goes, closed or dead, its windows go and intuition lets
 * go of their buffers' Areas. hwdb starts intuition when it's wanted
 * (decision 4): its test now, and in V8 the Workbench.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/intuition.h>
#include <planar/syscalls.h>

#include "compose.h"
#include "log.h"
#include "pointer.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_ACCEPT = 16, SIG_CONN = 17, SIG_LATCH = 18, SIG_DISPLAY = 19;
constexpr unsigned CONNS = 16;
constexpr uint64_t MS = 1000000;
constexpr uint64_t CALL_DEADLINE = 1000 * MS;   /* for display's answers (V6 step 7) */

/* A client's connection: its task, and its windows' sync words. */
struct client {
    Handle task;
    Handle page_area;               /* 0 until its first window */
    struct IntuiSync *page;
    uint32_t lines;                 /* a bit for each line in use */
};

struct window {
    bool used, shown;
    uint8_t conn, line;
    uint32_t buffers, cur;
    struct ic_window icw;
    struct BitMap bm[INTUI_BUFFERS];
    uint64_t area[INTUI_BUFFERS];   /* the Areas they lie in, as mapped here */
    uint64_t frame;                 /* presented last */
    uint64_t composed;              /* taken by the last frame composed */
    uint64_t latching;              /* in the flip waiting to latch */
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
    bool flipping, first;
    uint64_t flip_seq;

    uint64_t frames, nanos;
} it = { .first = true };

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
    return &it.cl[w->conn].page[w->line];
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
    uint64_t t;
    int64_t e;

    if (it.flipping || (!ic_dirty(&it.screen) && !unsafe()))
        return;
    if (!ic_dirty(&it.screen)) {        /* nothing on the screen changed */
        taken();
        shown(it.ds->ds_Time);
        return;
    }
    t = CurrentTime();
    ic_compose(&it.screen, &it.sbm[it.back]);
    it.nanos += CurrentTime() - t;
    it.frames++;
    taken();
    memset(it.dm, 0, sizeof *it.dm);
    it.dm->dm_Count = it.first ? 2 : 1;
    it.dm->dm_Set[0] = (struct DisplayPlane){ 1, it.buf[it.back], 0, 0 };
    it.dm->dm_Set[1] = (struct DisplayPlane){ 2, it.pointer, it.px, it.py };
    if ((e = display(DISPLAY_COMMIT)) != 0) {
        say("intuition: display refused a frame");
        ExitProcess(e == ERR_PORT_DIED || e == ERR_TIMEOUT ? 3 : 4);
    }
    it.first = false;
    it.flip_seq = it.dm->dm_Seq;
    it.flipping = true;
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

static struct window *find(unsigned c, uint32_t id)
{
    struct window *w;

    if (id == 0 || id > IC_WINDOWS)
        return nullptr;
    w = &it.win[id - 1];
    return w->used && w->conn == c ? w : nullptr;
}

/* Let go of the Area at 'area' unless another window's buffer lies in it. */
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
    ReleaseArea((const void *)area);
}

/* A buffer as OPEN describes it: in an Area another process shared
 * with intuition, readable, and holding all of it. */
static int64_t buffer(const struct IntuiBuffer *ib, uint32_t w, uint32_t h, struct BitMap *bm,
                      uint64_t *area)
{
    struct AreaInfo ai;
    uint64_t end;

    if (QueryArea((const void *)ib->ib_Address, &ai) != 0 || ai.ai_Area != 0 || ai.ai_Sharers < 2 ||
        !(ai.ai_Rights & AREAR_READ))
        return ERR_RIGHTS;      /* not shared with intuition: not mapped, its own, or the kernel's */
    *area = (uint64_t)ai.ai_Address;
    if (ib->ib_Width < w || ib->ib_Height < h ||
        !it.gfx->InitBitMap(bm, (void *)ib->ib_Address, ib->ib_Width, ib->ib_Height,
                            ib->ib_BytesPerRow, ib->ib_Format))
        return ERR_BADARG;
    end = ib->ib_Address + (uint64_t)ib->ib_BytesPerRow * (ib->ib_Height - 1) + (uint64_t)ib->ib_Width * 4;
    return end <= (uint64_t)ai.ai_Address + ai.ai_Size ? 0 : ERR_BADARG;
}

static int64_t page_up(unsigned c, Handle task)
{
    struct client *cl = &it.cl[c];
    int64_t h, e;
    void *at;

    if (cl->page_area)
        return 0;
    if ((h = AllocArea(4096, AREAF_CLEAR, &at)) < 0)
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
        q->im_Width > GFX_MAX_SIZE || q->im_Height > GFX_MAX_SIZE)
        e = ERR_BADARG;
    for (unsigned b = 0; b < q->im_Buffers && b < INTUI_BUFFERS && !e; b++)
        e = buffer(&q->im_Buffer[b], q->im_Width, q->im_Height, &n.bm[b], &n.area[b]);
    if (!e && (!w || line == INTUI_WINDOWS))
        e = ERR_FULL;
    if (!e)
        e = page_up(c, q->im_Task);
    if (e) {
        /* Refused: what was shared for it goes back, unless in use. */
        for (unsigned b = 0; b < INTUI_BUFFERS; b++)
            release(n.area[b], nullptr);
        return e;
    }
    n.line = (uint8_t)line;
    n.buffers = q->im_Buffers;
    n.icw = (struct ic_window){ q->im_Left, q->im_Top, q->im_Width, q->im_Height, nullptr };
    memcpy(n.title, q->im_Title, INTUI_TITLE);
    n.title[INTUI_TITLE - 1] = 0;
    *w = n;
    w->icw.bm = nullptr;
    cl->lines |= 1u << line;
    cl->page[line] = (struct IntuiSync){ 0 };
    m->im_Window = (uint32_t)(w - it.win) + 1;
    m->im_Sync = (uint64_t)&cl->page[line];
    return 0;
}

static void close_window(struct window *w)
{
    if (w->shown)
        ic_remove(&it.screen, &w->icw);
    it.cl[w->conn].lines &= ~(1u << w->line);
    w->used = false;
    for (unsigned b = 0; b < w->buffers; b++)
        release(w->area[b], w);
    *w = (struct window){ 0 };
}

static int64_t present(unsigned c, const struct IntuiMsg *q)
{
    struct window *w = find(c, q->im_Window);

    if (!w)
        return ERR_NOTFOUND;
    if (q->im_Index >= w->buffers || q->im_Count > INTUI_RECTS || q->im_Frame <= w->frame)
        return ERR_BADARG;
    for (unsigned i = 0; i < q->im_Count; i++) {
        const struct GfxRect *r = &q->im_Rect[i];

        if (r->MinX < 0 || r->MinY < 0 || r->MinX > r->MaxX || r->MinY > r->MaxY ||
            r->MaxX >= (int64_t)w->icw.w || r->MaxY >= (int64_t)w->icw.h)
            return ERR_BADARG;
    }
    w->cur = q->im_Index;
    w->frame = q->im_Frame;
    w->icw.bm = &w->bm[w->cur];
    if (!w->shown) {
        w->shown = ic_add(&it.screen, &w->icw);
        return w->shown ? 0 : ERR_FULL;
    }
    if (!q->im_Count)
        ic_damage_window(&it.screen, &w->icw, (struct GfxRect){ 0, 0, (int32_t)w->icw.w - 1,
                                                                (int32_t)w->icw.h - 1 });
    for (unsigned i = 0; i < q->im_Count; i++)
        ic_damage_window(&it.screen, &w->icw, q->im_Rect[i]);
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

    for (unsigned i = 0; i < IC_WINDOWS; i++)
        if (it.win[i].used && it.win[i].conn == c)
            close_window(&it.win[i]);
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
    if ((h = CreatePort("intuition", SIG_ACCEPT)) < 0) {
        say("intuition: \"intuition\" couldn't be published");
        return 1;
    }
    it.port = (Handle)h;
    KPrintF("intuition: the screen, %lux%lu, composed on display's plane 1, the pointer on 2",
            (uint64_t)it.width, (uint64_t)it.height);
    for (;;) {
        frame();
        WaitPorts(it.mp, it.nconns, 1ULL << SIG_ACCEPT | 1ULL << SIG_CONN | 1ULL << SIG_LATCH);
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
        latch();
    }
}
