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
 *    should be none: each latched at the next. Between the two, the
 *    last flip's overlay read back from the screen (DISPLAY_READ, v7-plan
 *    step 1) where display composes it: its CRC-32 as drawn. Then the
 *    other overlay drawn by gfx.library (v7-plan step 2) and shown, read
 *    back the same way against gfxref.h's drawing of it. Then it shows
 *    them, allocates a fourth buffer to say so, and waits to be killed:
 *    hwdb checks display freed its buffers.
 * 1: shows an overlay and the cursor, allocates a third buffer to say
 *    so, and waits for display to die under them (hwdb kills it). Its buffers stay mapped and writable,
 *    held until the DPU has let go of them; it connects to the restarted
 *    display, which shows none of them, and ends with 0.
 *
 * It ends by itself only when done, or with a failed check's number.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/gfx.h>
#include <planar/syscalls.h>

#include "gfxref.h"
#include "log.h"

constexpr int64_t SIG_DISPLAY = 20, SIG_TIMER = 21;
constexpr uint64_t MS = 1000000;
constexpr unsigned FLIPS = 30, PACED = 60;

static unsigned step;

static int failed(const char *what)
{
    KPrintF("displaytest: check %lu failed: %s", (uint64_t)step, what);
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

/* Connected, waiting for its port until 'until' (0: not at all). */
static int64_t connect(uint64_t until)
{
    int64_t e = ConnectPortUntil(&mp, "display.0", SIG_DISPLAY, 4, 2 * sizeof *m, until);

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

/* The screen's rectangle as DISPLAY_READ sums it: 0 and *e if refused. */
static uint32_t read_screen(int32_t x, int32_t y, uint32_t w, uint32_t h, int64_t *e)
{
    memset(m, 0, sizeof *m);
    m->dm_Set[0] = (struct DisplayPlane){ .dp_X = x, .dp_Y = y };
    m->dm_Width = w;
    m->dm_Height = h;
    return (*e = ask(DISPLAY_READ)) == 0 ? m->dm_Crc : 0;
}

/* What it should be: the rectangle (0, 0, w, h) of draw()'s bars. */
static uint32_t bars_crc(uint32_t rgb, uint32_t w, uint32_t h)
{
    uint32_t crc = ~0u;

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = (x / 16 + y / 16) & 1 ? rgb : 0;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

/* gfx.library's overlay: a panel, its bevel, a label, and a translucent
 * badge blitted over them; by IGfx, or by the reference, the same. */
static void panel(const struct GfxIFace *g, struct RastPort *rp, const struct BitMap *badge)
{
    rp->rp_FgPen = 0x203040;
    g->RectFill(rp, 0, 0, 255, 127);
    g->DrawBevel(rp, 4, 4, 120, 27, 0xe0e0e0, 0x404040);
    rp->rp_FgPen = 0xffff80;
    rp->rp_BgPen = 0x203040;
    g->Text(rp, 10, 10, "Planar gfx", 10);
    g->BltBitMap(badge, 0, 0, rp, 90, 6, 32, 20, BLEND_SRC_OVER);
}

/* The CRC-32 DISPLAY_READ gives of (0, 0, w, h) of a bitmap: the top
 * byte as 0. */
static uint32_t bitmap_crc(const struct BitMap *bm, uint32_t w, uint32_t h)
{
    uint32_t crc = ~0u;

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = *gr_px(bm, x, y) & 0xffffff;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

/* Buffer a, drawn by gfx.library and shown on plane 1 at (40, 40): what
 * the screen shows of it where display composes it is what the
 * reference draws. */
static int gfx_on_screen(const struct buf *a, bool readable)
{
    static uint32_t want[32 * 128];
    struct Library *lib = IExec->OpenLibrary("gfx.library", 7);
    struct GfxIFace *IGfx = lib ? (struct GfxIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
    struct BitMap screen, ref, *badge;
    struct RastPort rp;
    uint64_t seq;
    int64_t e;

    CHECK(IGfx != nullptr);
    CHECK((badge = IGfx->AllocBitMap(32, 20, PIXFMT_ARGB8888, 0, nullptr)) != nullptr);
    IGfx->InitRastPort(&rp, badge);
    for (int32_t y = 0; y < 20; y++) {
        rp.rp_FgPen = (uint32_t)(y * 13) << 24 | 0xc02060;      /* alpha 0 to 247, down it */
        IGfx->RectFill(&rp, 0, y, 31, y);
    }
    CHECK(IGfx->InitBitMap(&screen, (void *)a->px, a->width, a->height, a->stride, PIXFMT_XRGB8888));
    IGfx->InitRastPort(&rp, &screen);
    panel(IGfx, &rp, badge);
    CHECK((seq = commit(1, a->id, 40, 40, 0, 0, 0)) != 0);
    CHECK(shown(seq));
    if (readable) {
        CHECK(IGfx->InitBitMap(&ref, want, 128, 32, 128 * 4, PIXFMT_XRGB8888));
        IGfx->InitRastPort(&rp, &ref);
        panel(&gr_iface, &rp, badge);
        CHECK(read_screen(40, 40, 128, 32, &e) == bitmap_crc(&ref, 128, 32) && e == 0);
    }
    IGfx->FreeBitMap(badge);
    return 0;
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

static void fmt_range(char *out, size_t size, const char *what, uint64_t lo, uint64_t hi)
{
    SNPrintf(out, size, "%s%lu-%lu us", what, lo / 1000, hi / 1000);
}

/* PACED flips, each committed just after a vblank: when each latched. */
static int paced(const struct buf *a, const struct buf *b, const struct buf *c)
{
    uint64_t commit_lo = UINT64_MAX, commit_hi = 0, vsync_lo = UINT64_MAX, vsync_hi = 0;
    uint64_t missed = 0;
    char commit_s[64], vsync_s[64];

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
    fmt_range(commit_s, sizeof commit_s, "commit to latch ", commit_lo, commit_hi);
    fmt_range(vsync_s, sizeof vsync_s, ", vblank to latch ", vsync_lo, vsync_hi);
    KPrintF("displaytest: %lu flips paced by the vblank: %s%s, %lu vblanks missed",
            (uint64_t)PACED, commit_s, vsync_s, missed);
    return 0;
}

static int flips(void)
{
    int r;

    struct buf a, b, c, d;
    uint64_t seq, worst = 0, total = 0, vb0, vblanks;
    char mode[64] = "";
    int64_t e;
    uint32_t crc;
    bool readable;

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
    /* The last flip's overlay (b, at 40, 40) as the screen shows it, left
     * of the cursor: read back where display composes the screen, and
     * refused where a controller scans it out. */
    crc = read_screen(40, 40, 128, 32, &e);
    CHECK(e == 0 || e == ERR_NOSYS);
    if ((readable = e == 0)) {
        CHECK(crc == bars_crc(0x40c040, 128, 32));
        CHECK(read_screen(40, 40, 128, 32, &e) == crc);
        CHECK(read_screen(41, 40, 128, 32, &e) != crc);
        read_screen(-1, 0, 1, 1, &e);
        CHECK(e == ERR_BADARG);
    }
    /* A buffer shown can't be freed; one that isn't can. */
    memset(m, 0, sizeof *m);
    m->dm_Buffer = b.id;
    CHECK(ask(DISPLAY_FREE) == ERR_BUSY);
    CHECK(alloc(&d, 64, 64, DISPLAY_XRGB8888) == 0);
    memset(m, 0, sizeof *m);
    m->dm_Buffer = d.id;
    CHECK(ask(DISPLAY_FREE) == 0);
    CHECK(ask(DISPLAY_FREE) == ERR_NOTFOUND);
    if ((r = gfx_on_screen(&a, readable)) != 0)
        return r;

    vblanks = sync_page->ds_Vblank - vb0;
    memset(m, 0, sizeof *m);
    if (ask(DISPLAY_INFO) == 0 && m->dm_RefreshMilliHz)
        SNPrintf(mode, sizeof mode, " at %lu.%lu Hz, %lux%lu",
                 (uint64_t)(m->dm_RefreshMilliHz / 1000),
                 (uint64_t)(m->dm_RefreshMilliHz / 100 % 10),
                 (uint64_t)m->dm_Width, (uint64_t)m->dm_Height);
    KPrintF("displaytest: %lu flips, each on the screen %lu us after its commit (worst %lu us), "
            "over %lu vblanks%s", (uint64_t)FLIPS, (uint64_t)(total / FLIPS / 1000),
            (uint64_t)(worst / 1000), vblanks, mode);
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
    while (connect(until) != 0)         /* its port, as soon as it's back */
        CHECK(CurrentTime() < until);
    CHECK(m->dm_Buffers == 0);
    CHECK(sync_page->ds_Disp == 0);
    draw(&a, 0x202020, false);
    KLog("displaytest: display came back; our buffers stayed writable, and it shows none of them");
    return 0;
}

int main(uint64_t arg)
{
    CHECK(connect(0) == 0);
    return arg == 0 ? flips() : outlive();
}
