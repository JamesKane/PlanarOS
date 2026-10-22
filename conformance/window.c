/*
 * conformance/window.c -- intuition's windows, presented (v7-plan step 3)
 *
 * Started by hwdb's window test (servers/hwdb/intuition.c) once it has
 * started intuition. Through intuition.library, with bitmaps from
 * gfx.library:
 *
 * 1. Two windows opened, overlapping: one double-buffered, one not. Each
 *    presented whole, then the first in part (its back buffer brought up
 *    to date first), and the screen read back (DISPLAY_READ, where
 *    display composes it) against the windows as this program drew them
 *    over the backdrop, and the pointer.
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
 * 4. Both closed, and the backdrop read back where they were.
 *
 * Then the costs, intuition's own: composing, a megapixel. Exits 0, or
 * the failed check's number.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/intuition.h>
#include <planar/syscalls.h>

#include "../servers/intuition/pointer.h"
#include "log.h"

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_DISPLAY = 20;
constexpr uint64_t MS = 1000000;
constexpr unsigned FRAMES = 120, TIMED = 10;
constexpr uint32_t BACKDROP = 0x182038;     /* intuition's (compose.h's IC_BACKDROP) */

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

static int64_t display_up(void)
{
    int64_t e = ConnectPortUntil(&dmp, "display.0", SIG_DISPLAY, 2, 2 * sizeof *dm, 0);

    if (e != 0)
        return e;
    if (!(dm = AllocMsg(&dmp, sizeof *dm)))
        return ERR_NOMEM;
    memset(dm, 0, sizeof *dm);
    if ((e = display(DISPLAY_INFO)) != 0)
        return e;
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
static unsigned open_windows;       /* of wa and wb, front last: 2 both, 1 wa alone, 0 none */

static uint32_t shown_px(const struct Window *w, int32_t x, int32_t y)
{
    const struct BitMap *bm = w->w_BitMap[(w->w_Back + w->w_Buffers - 1) % w->w_Buffers];

    return ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x];
}

static bool inside(const struct Window *w, int32_t x, int32_t y)
{
    return x >= w->w_LeftEdge && y >= w->w_TopEdge && x < w->w_LeftEdge + (int32_t)w->w_Width &&
           y < w->w_TopEdge + (int32_t)w->w_Height;
}

static uint32_t screen_px(int32_t x, int32_t y)
{
    int32_t px = (int32_t)in.in_Width / 2, py = (int32_t)in.in_Height / 2;  /* intuition's */
    uint32_t p;

    if (x >= px && y >= py && (p = pointer_pixel((uint32_t)(x - px), (uint32_t)(y - py))) >> 24)
        return p;
    if (open_windows >= 2 && inside(&wb, x, y))
        return shown_px(&wb, x - wb.w_LeftEdge, y - wb.w_TopEdge);
    if (open_windows >= 1 && inside(&wa, x, y))
        return shown_px(&wa, x - wa.w_LeftEdge, y - wa.w_TopEdge);
    return BACKDROP;
}

static uint32_t expect(int32_t x0, int32_t y0, uint32_t w, uint32_t h)
{
    uint32_t crc = ~0u;

    for (int32_t y = y0; y < y0 + (int32_t)h; y++)
        for (int32_t x = x0; x < x0 + (int32_t)w; x++) {
            uint32_t px = screen_px(x, y) & 0xffffff;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

/* The rectangle as the screen shows it, a second at most to get there:
 * true if it did, or the screen can't be read here. */
static bool screen_is(bool readable, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    uint64_t until = CurrentTime() + 1000 * MS;
    uint32_t want = expect(x, y, w, h), got;

    if (!readable)
        return true;
    do
        if (read_screen(x, y, w, h, &got) == 0 && got == want)
            return true;
    while (CurrentTime() < until);
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
    CHECK((lib = IExec->OpenLibrary("intuition.library", 7)) && lib->lib_Version == 7);
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
                         { WA_BitMap, (uint64_t)bm[0] }, { WA_BackBitMap, (uint64_t)bm[1] }) == 0);
    CHECK(OpenWindowTags(IIntuition, &in, &wb, { WA_Left, 180 }, { WA_Top, 100 },
                         { WA_BitMap, (uint64_t)bm[2] }) == 0);
    CHECK(wa.w_Width == 200 && wa.w_Height == 120 && wa.w_Buffers == 2 && wb.w_Buffers == 1);
    picture(&wa.w_RPort, 200, 120, 0x5b88c2, "window a");
    picture(&wb.w_RPort, 160, 100, 0x4a5668, "window b");
    CHECK(IIntuition->PresentWindow(&wa, nullptr, 0) == 0);
    CHECK(IIntuition->PresentWindow(&wb, nullptr, 0) == 0);
    open_windows = 2;
    CHECK(wa.w_Frame == 1 && wa.w_Back == 1 && wb.w_Back == 0);
    CHECK(on_screen(&wa, 1) && on_screen(&wb, 1));
    CHECK(IIntuition->WaitSafe(&wa, CurrentTime() + 1000 * MS) == 0);
    e = read_screen(32, 32, 416, 240, &crc);
    *readable = e == 0;
    CHECK(e == 0 || e == ERR_NOSYS);
    CHECK(screen_is(*readable, 32, 32, 416, 240));
    CHECK(screen_is(*readable, (int32_t)in.in_Width / 2 - 8, (int32_t)in.in_Height / 2 - 8, 32, 32));

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
                         { WA_BitMap, (uint64_t)bm[2] }) == 0);
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

int main(uint64_t)
{
    struct BitMap *bm[3];
    struct AreaInfo ai;
    bool readable;
    int r;

    if ((r = open_lib()))
        return r;
    if ((r = (int)IIntuition->OpenIntuition(&in, CurrentTime() + 5000 * MS)) != 0)
        KPrintF("window: intuition wouldn't connect: %ld", (int64_t)r);
    CHECK(r == 0);
    CHECK(in.in_Width >= 640 && in.in_Height >= 480);
    CHECK(display_up() == 0);
    if ((r = two_windows(&readable, bm)) || (r = refusals(bm)) || (r = frames(readable)))
        return r;

    /* 4: closed, and the backdrop where they were; intuition maps their
     * bitmaps while they're open, and not after. */
    for (unsigned i = 0; i < 3; i++)
        CHECK(QueryArea(bm[i]->bm_BaseAddr, &ai) == 0 && ai.ai_Sharers == 2);
    CHECK(IIntuition->CloseWindow(&wb) == 0);
    open_windows = 1;
    CHECK(screen_is(readable, 32, 32, 416, 240));
    CHECK(IIntuition->CloseWindow(&wa) == 0);
    CHECK(IIntuition->CloseWindow(&wa) == ERR_BADARG);
    open_windows = 0;
    CHECK(screen_is(readable, 32, 32, 416, 240));
    for (unsigned i = 0; i < 3; i++)
        CHECK(QueryArea(bm[i]->bm_BaseAddr, &ai) == 0 && ai.ai_Sharers == 1);

    memset(in.in_Call, 0, sizeof *in.in_Call);
    CHECK(ask(&in, in.in_Call, INTUI_INFO) == 0 && in.in_Call->im_Frames > 0);
    KPrintF("window: intuition composed %lu frames, %lu us a megapixel; the screen %s",
            in.in_Call->im_Frames, in.in_Call->im_Nanos * 1000 / (in.in_Call->im_Pixels + 1),
            readable ? "read back as drawn at each step" : "not read back (display's controller)");
    IIntuition->CloseIntuition(&in);
    for (unsigned i = 0; i < 3; i++)
        IGfx->FreeBitMap(bm[i]);
    return 0;
}
