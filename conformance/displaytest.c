/*
 * conformance/displaytest.c -- a client of display's planes (v5-plan step 3)
 *
 * Started by hwdb's display test (servers/hwdb/display.c), as intuition
 * will be, with one argument:
 *
 * 0: checks the refusals, then flips an overlay between two buffers 30
 *    times with the cursor moving over it, each commit waited for in
 *    the sync page, and says how long they took. Then PACED flips, each
 *    committed just after a vblank, timed as omega did (v5-plan step 7):
 *    commit to latch, vblank to latch, and the vblanks missed, which
 *    should be none: each latched at the next. Then it shows them,
 *    allocates a fourth buffer to say so, and waits to be killed: hwdb
 *    checks display freed its buffers.
 * 1: shows an overlay and the cursor, allocates a third buffer to say
 *    so, and waits for display to die under them (hwdb kills it). Its buffers stay mapped and writable,
 *    held until the DPU has let go of them; it connects to the restarted
 *    display, which shows none of them, and ends with 0.
 *
 * It ends by itself only when done, or with a failed check's number.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/syscalls.h>

#include "log.h"

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_DISPLAY = 20, SIG_TIMER = 21;
constexpr uint64_t MS = 1000000;
constexpr unsigned FLIPS = 30, PACED = 60;

static unsigned step;

static int failed(const char *what)
{
    struct logbuf b = { 0 };

    log_str(&b, "displaytest: check ");
    log_num(&b, step);
    log_str(&b, " failed: ");
    log_str(&b, what);
    KLog(b.text);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static struct MsgPort mp;
static struct DisplayMsg *m;
static const struct DisplaySync *sync_page;

static int64_t ask(uint32_t op)
{
    m->dm_Msg.mn_Length = sizeof *m;
    m->dm_Op = op;
    m->dm_Task = FindTask(nullptr);
    if (DoMsg(&mp, &m->dm_Msg) != 0)
        return ERR_PORT_DIED;
    return m->dm_Msg.mn_Error;
}

static int64_t connect(void)
{
    int64_t e = ConnectPort(&mp, "display.0", SIG_DISPLAY, 4, 2 * sizeof *m);

    if (e != 0)
        return e;
    if (!(m = AllocMsg(&mp, sizeof *m)))
        return ERR_NOMEM;
    memset(m, 0, sizeof *m);
    if ((e = ask(DISPLAY_INFO)) != 0)
        return e;
    sync_page = (const struct DisplaySync *)m->dm_Sync;
    return 0;
}

struct buf {
    uint32_t id, stride, width, height;
    volatile uint32_t *px;
};

static int64_t alloc(struct buf *b, uint32_t w, uint32_t h, uint32_t format)
{
    int64_t e;

    memset(m, 0, sizeof *m);
    m->dm_Width = w;
    m->dm_Height = h;
    m->dm_Format = format;
    if ((e = ask(DISPLAY_ALLOC)) != 0)
        return e;
    *b = (struct buf){ m->dm_Buffer, m->dm_Stride, w, h, (volatile uint32_t *)m->dm_Address };
    return 0;
}

/* Bars of 'rgb' on black, or the cursor: an arrow-ish wedge, clear
 * outside it. */
static void draw(const struct buf *b, uint32_t rgb, bool cursor)
{
    for (uint32_t y = 0; y < b->height; y++)
        for (uint32_t x = 0; x < b->width; x++)
            b->px[y * (b->stride / 4) + x] =
                cursor ? (x <= y && x + y < 2 * b->height - 4 ? 0xffffffff : 0x00000000)
                       : ((x / 16 + y / 16) & 1 ? rgb : 0x000000);
}

/* Commit plane p to buffer 'id' at (x, y), with the cursor's at (cx, cy)
 * if 'cursor': its number, or 0. */
static uint64_t commit(uint32_t p, uint32_t id, int32_t x, int32_t y, uint32_t cursor, int32_t cx,
                       int32_t cy)
{
    memset(m, 0, sizeof *m);
    m->dm_Count = cursor ? 2 : 1;
    m->dm_Set[0] = (struct DisplayPlane){ p, id, x, y };
    m->dm_Set[1] = (struct DisplayPlane){ 2, cursor, cx, cy };
    return ask(DISPLAY_COMMIT) == 0 ? m->dm_Seq : 0;
}

/* Until the commit is on the screen, a quarter-second at most. */
static bool shown(uint64_t seq)
{
    uint64_t until = CurrentTime() + 250 * MS, v;

    while ((v = sync_page->ds_Disp) < seq)
        if (WaitAddr(&sync_page->ds_Disp, v, until) == ERR_TIMEOUT)
            return sync_page->ds_Disp >= seq;
    return true;
}

/* The last vblank: its number and its time, read together. */
static uint64_t last_vblank(uint64_t *at)
{
    uint64_t v;

    do {
        v = sync_page->ds_Vblank;
        *at = sync_page->ds_Time;
    } while (sync_page->ds_Vblank != v);
    return v;
}

static void log_range(struct logbuf *l, const char *what, uint64_t lo, uint64_t hi)
{
    log_str(l, what);
    log_num(l, lo / 1000);
    log_str(l, "-");
    log_num(l, hi / 1000);
    log_str(l, " us");
}

/* PACED flips, each committed just after a vblank: when each latched. */
static int paced(const struct buf *a, const struct buf *b, const struct buf *c)
{
    uint64_t commit_lo = UINT64_MAX, commit_hi = 0, vsync_lo = UINT64_MAX, vsync_hi = 0;
    uint64_t missed = 0;
    struct logbuf l = { 0 };

    for (unsigned i = 0; i < PACED; i++) {
        uint64_t v0, t0, v1, t1, t_commit, seq, until = CurrentTime() + 250 * MS;

        /* A vblank, just now. */
        v0 = sync_page->ds_Vblank;
        while (sync_page->ds_Vblank == v0)
            CHECK(WaitAddr(&sync_page->ds_Vblank, v0, until) != ERR_TIMEOUT ||
                  sync_page->ds_Vblank != v0);
        v0 = last_vblank(&t0);
        CHECK((seq = commit(1, i & 1 ? b->id : a->id, 40, 40, c->id, (int32_t)(i * 4), 80)) != 0);
        t_commit = CurrentTime();
        CHECK(shown(seq));
        v1 = last_vblank(&t1);
        CHECK(v1 > v0);                 /* latched at a vblank after the commit */
        if (t1 - t_commit < commit_lo)
            commit_lo = t1 - t_commit;
        if (t1 - t_commit > commit_hi)
            commit_hi = t1 - t_commit;
        if (t1 - t0 < vsync_lo)
            vsync_lo = t1 - t0;
        if (t1 - t0 > vsync_hi)
            vsync_hi = t1 - t0;
        missed += v1 - v0 - 1;
    }
    log_str(&l, "displaytest: ");
    log_num(&l, PACED);
    log_str(&l, " flips paced by the vblank: ");
    log_range(&l, "commit to latch ", commit_lo, commit_hi);
    log_range(&l, ", vblank to latch ", vsync_lo, vsync_hi);
    log_str(&l, ", ");
    log_num(&l, missed);
    log_str(&l, " vblanks missed");
    KLog(l.text);
    return 0;
}

static int flips(void)
{
    int r;

    struct buf a, b, c, d;
    uint64_t seq, worst = 0, total = 0, vb0;
    struct logbuf l = { 0 };

    /* What it is, and refusals. */
    CHECK(m->dm_Width > 0 && m->dm_Height > 0 && m->dm_Count >= 3 && m->dm_Count <= DISPLAY_PLANES);
    CHECK(m->dm_Plane[0].di_Flags & DISPLAYP_CONSOLE);
    CHECK(m->dm_Plane[2].di_Flags & DISPLAYP_CURSOR);
    CHECK(m->dm_Plane[1].di_Formats & 1u << DISPLAY_XRGB8888);
    CHECK(m->dm_Plane[2].di_Formats & 1u << DISPLAY_ARGB8888);
    memset(m, 0, sizeof *m);
    CHECK(ask(DISPLAY_ALLOC) == ERR_BADARG);                /* no size */
    m->dm_Width = m->dm_Height = 16;
    m->dm_Format = 99;
    CHECK(ask(DISPLAY_ALLOC) == ERR_BADARG);
    CHECK(alloc(&a, 256, 128, DISPLAY_XRGB8888) == 0);
    CHECK(alloc(&b, 256, 128, DISPLAY_XRGB8888) == 0);
    CHECK(alloc(&c, 32, 32, DISPLAY_ARGB8888) == 0);
    CHECK(a.stride >= 256 * 4 && a.stride % 64 == 0);
    memset(m, 0, sizeof *m);
    m->dm_Count = 1;
    m->dm_Set[0] = (struct DisplayPlane){ 0, a.id, 0, 0 };
    CHECK(ask(DISPLAY_COMMIT) == ERR_RIGHTS);               /* the console's */
    m->dm_Set[0] = (struct DisplayPlane){ 1, 77, 0, 0 };
    CHECK(ask(DISPLAY_COMMIT) == ERR_NOTFOUND);
    m->dm_Set[0] = (struct DisplayPlane){ 9, a.id, 0, 0 };
    CHECK(ask(DISPLAY_COMMIT) == ERR_BADARG);
    draw(&a, 0xc04040, false);
    draw(&b, 0x40c040, false);
    draw(&c, 0, true);

    /* The flips, the cursor walking across, partly off the left edge at
     * first: each on the screen by the next vblank or so. */
    vb0 = sync_page->ds_Vblank;
    for (unsigned i = 0; i < FLIPS; i++) {
        uint64_t t = CurrentTime(), dt;

        CHECK((seq = commit(1, i & 1 ? b.id : a.id, 40, 40, c.id, (int32_t)(i * 8) - 16, 80)) != 0);
        CHECK(shown(seq));
        CHECK(sync_page->ds_Safe >= seq);
        dt = CurrentTime() - t;
        total += dt;
        if (dt > worst)
            worst = dt;
    }
    CHECK(sync_page->ds_Vblank - vb0 >= FLIPS);
    /* A buffer shown can't be freed; one that isn't can. */
    memset(m, 0, sizeof *m);
    m->dm_Buffer = b.id;
    CHECK(ask(DISPLAY_FREE) == ERR_BUSY);
    CHECK(alloc(&d, 64, 64, DISPLAY_XRGB8888) == 0);
    memset(m, 0, sizeof *m);
    m->dm_Buffer = d.id;
    CHECK(ask(DISPLAY_FREE) == 0);
    CHECK(ask(DISPLAY_FREE) == ERR_NOTFOUND);

    log_str(&l, "displaytest: ");
    log_num(&l, FLIPS);
    log_str(&l, " flips, each on the screen ");
    log_num(&l, total / FLIPS / 1000);
    log_str(&l, " us after its commit (worst ");
    log_num(&l, worst / 1000);
    log_str(&l, " us), over ");
    log_num(&l, sync_page->ds_Vblank - vb0);
    log_str(&l, " vblanks");
    memset(m, 0, sizeof *m);
    if (ask(DISPLAY_INFO) == 0 && m->dm_RefreshMilliHz) {
        log_str(&l, " at ");
        log_num(&l, m->dm_RefreshMilliHz / 1000);
        log_str(&l, ".");
        log_num(&l, m->dm_RefreshMilliHz / 100 % 10);
        log_str(&l, " Hz, ");
        log_num(&l, m->dm_Width);
        log_str(&l, "x");
        log_num(&l, m->dm_Height);
    }
    KLog(l.text);
    if ((r = paced(&a, &b, &c)) != 0)
        return r;
    /* A fourth buffer says we're done (hwdb counts them); shown until
     * hwdb kills us. */
    CHECK(alloc(&d, 16, 16, DISPLAY_XRGB8888) == 0);
    for (;;)
        Wait(1ULL << 40);
}

static int outlive(void)
{
    struct buf a, c, d;
    uint64_t seq, until;

    CHECK(alloc(&a, 200, 100, DISPLAY_XRGB8888) == 0);
    CHECK(alloc(&c, 32, 32, DISPLAY_ARGB8888) == 0);
    draw(&a, 0x4040c0, false);
    draw(&c, 0, true);
    CHECK((seq = commit(1, a.id, 100, 100, c.id, 150, 150)) != 0);
    CHECK(shown(seq));
    CHECK(alloc(&d, 16, 16, DISPLAY_XRGB8888) == 0);    /* a third: we're ready */
    KLog("displaytest: showing two planes; waiting for display to die");

    /* display dies (hwdb kills it): the connection goes. */
    until = CurrentTime() + 20000 * MS;
    while (!PortGone(&mp)) {
        CHECK(CurrentTime() < until);
        ArmTimer(CurrentTime() + 20 * MS, SIG_TIMER);
        Wait(1ULL << SIG_DISPLAY | 1ULL << SIG_TIMER);
    }
    ClosePort(&mp);
    /* Its buffers are still ours to write: held, not freed. */
    draw(&a, 0x808080, false);
    /* It comes back, with none of them. */
    until = CurrentTime() + 10000 * MS;
    while (connect() != 0) {
        CHECK(CurrentTime() < until);
        ArmTimer(CurrentTime() + 50 * MS, SIG_TIMER);
        Wait(1ULL << SIG_TIMER);
    }
    CHECK(m->dm_Buffers == 0);
    CHECK(sync_page->ds_Disp == 0);
    draw(&a, 0x202020, false);
    KLog("displaytest: display came back; our buffers stayed writable, and it shows none of them");
    return 0;
}

int main(uint64_t arg)
{
    CHECK(connect() == 0);
    return arg == 0 ? flips() : outlive();
}
