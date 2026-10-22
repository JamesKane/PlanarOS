/*
 * conformance/window.c -- intuition's windows, presented, worked by the
 * mouse and keys, and outliving the servers (v7-plan steps 3 to 5)
 *
 * Started by hwdb's window test (servers/hwdb/intuition.c) once it has
 * started intuition. Through intuition.library, with bitmaps from
 * gfx.library:
 *
 * 1. Two windows opened, borderless, overlapping: one double-buffered,
 *    one not. Each presented whole, then the first in part (its back
 *    buffer brought up to date first), and the screen read back
 *    (DISPLAY_READ, where display composes it) against the windows as
 *    this program drew them over the backdrop, and the pointer.
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
 * 5. The mouse and keys, which it asks for ("window: send ..."), as
 *    inputtest does: on QEMU the boot check sends them through QMP.
 *    Two windows with chrome: focus moved by a click, each event where
 *    it should be and the window clicked brought to the front; keys at
 *    the active window alone; a window dragged by its title bar, sent
 *    back and brought forward by its depth gadget, and closed by its
 *    close gadget's IDCMP_CLOSEWINDOW. The chrome isn't read back, only
 *    the windows' insides and the backdrop.
 * 6. A client that has stopped (this program, started again with
 *    STOPPED): its window dragged and raised all the same, and taken off
 *    the screen a second after its close gadget, which it never answers.
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
 *    again, the pointer where it was, and (SURVIVE_MOUSE, if 5 ran) a
 *    click heard after. Exits 0, or the failed check's number.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/intuition.h>
#include <planar/syscalls.h>

#include "../servers/intuition/pointer.h"
#include "log.h"

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_DISPLAY = 20, SIG_HUNG = 21, SIG_KILL = 23;     /* 22 is intuition.library's */
constexpr uint64_t MS = 1000000;
constexpr unsigned FRAMES = 120, TIMED = 10;
constexpr uint32_t BACKDROP = 0x182038;     /* intuition's (compose.h's IC_BACKDROP) */
constexpr uint32_t CHROME = 0x00c0ffee;     /* the model's: a pixel of the chrome, not read; clear, so no pixel drawn */
constexpr int NO_INPUT = 1000;
enum : uint64_t { FIRST, STOPPED, KILLED, SURVIVE, SURVIVE_MOUSE = 4 };     /* the argument's low byte */

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
    int32_t left, top, bl, bt, iw, ih;
};

static struct place place_of(const void *k)
{
    const struct other *o = other_of(k);
    const struct Window *w = k;

    if (o)
        return (struct place){ o->x, o->y, o->chrome ? (int32_t)INTUI_BORDER : 0,
                               o->chrome ? (int32_t)INTUI_TITLEBAR : 0, (int32_t)o->w, (int32_t)o->h };
    return (struct place){ w->w_Sync->is_LeftEdge, w->w_Sync->is_TopEdge, w->w_BorderLeft,
                           w->w_BorderTop, (int32_t)w->w_Width, (int32_t)w->w_Height };
}

static uint32_t shown_px(const struct Window *w, int32_t x, int32_t y)
{
    const struct BitMap *bm = w->w_BitMap[(w->w_Back + w->w_Buffers - 1) % w->w_Buffers];

    return ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x];
}

/* (x, y) of the screen, from window 'k' of the model: false if it's not on it. */
static bool from(const void *k, int32_t x, int32_t y, uint32_t *px)
{
    struct place p = place_of(k);
    const struct other *o = other_of(k);
    int32_t br = p.bl ? (int32_t)INTUI_BORDER : 0;

    if (x < p.left || y < p.top || x >= p.left + p.bl + p.iw + br || y >= p.top + p.bt + p.ih + br)
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

    if (x >= ptr_x && y >= ptr_y && (p = pointer_pixel((uint32_t)(x - ptr_x), (uint32_t)(y - ptr_y))) >> 24)
        return p;
    for (unsigned i = 0; i < nshown; i++)
        if (from(order[i], x, y, &p))
            return p;
    return BACKDROP;
}

/* The checksum DISPLAY_READ would give; false if the rectangle holds
 * chrome, which the model doesn't know (a test's mistake). */
static bool expect(int32_t x0, int32_t y0, uint32_t w, uint32_t h, uint32_t *sum)
{
    uint32_t crc = ~0u;

    for (int32_t y = y0; y < y0 + (int32_t)h; y++)
        for (int32_t x = x0; x < x0 + (int32_t)w; x++) {
            uint32_t px = screen_px(x, y);

            if (px == CHROME)
                return false;
            px &= 0xffffff;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
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
                         { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)bm[0] },
                         { WA_BackBitMap, (uint64_t)bm[1] }) == 0);
    CHECK(OpenWindowTags(IIntuition, &in, &wb, { WA_Left, 180 }, { WA_Top, 100 },
                         { WA_Flags, WFLG_BORDERLESS }, { WA_BitMap, (uint64_t)bm[2] }) == 0);
    CHECK(wa.w_Width == 200 && wa.w_Height == 120 && wa.w_Buffers == 2 && wb.w_Buffers == 1);
    picture(&wa.w_RPort, 200, 120, 0x5b88c2, "window a");
    picture(&wb.w_RPort, 160, 100, 0x4a5668, "window b");
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
    }
    return e;
}

/* The pointer to (x, y), asked for 100 pixels at a time: false if it
 * didn't move within 2 s of asking (nobody's there to move it). */
static bool point_at(int32_t x, int32_t y)
{
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
    CHECK(gadget_click(HUNG, CLOSE_GADGET));            /* never answered */
    for (uint64_t until = CurrentTime() + 3000 * MS; info(m) == 0 && m->im_Windows != windows;)
        if (CurrentTime() > until)
            return failed("the stopped client's window was never taken off");
        else
            nap(10);
    taken_off(HUNG);
    CHECK(screen_is(readable, 408, 146, 76, 80));       /* the backdrop, right of d */
    ends();
    CHECK(IIntuition->CloseWindow(&wd) == 0);
    taken_off(&wd);
    IGfx->FreeBitMap(cbm);
    IGfx->FreeBitMap(dbm);
    KPrintF("window: input: a stopped client's window dragged and raised, and taken off a second "
            "after its close gadget");
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

int main(uint64_t arg)
{
    struct BitMap *bm[3];
    struct AreaInfo ai;
    bool readable;
    int r, input;

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
    if ((r = two_windows(&readable, bm)) || (r = refusals(bm)) || (r = frames(readable)))
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
