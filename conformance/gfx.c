/*
 * conformance/gfx.c -- gfx.library through its interface (V7.2)
 *
 * IGfx from OpenLibrary, then conformance/gfxcheck.h's cases through it:
 * every operation against the reference, to the bit. On a CPU with a
 * vector unit (QEMU's, both ISAs) the table's RectFill, BltBitMap and
 * DrawBevel are RVV's or NEON's by then, so this is where RVV's are
 * checked (lib/gfx/test.c checks NEON's on the host too). Then a bitmap
 * drawn here and shared read-only with a second process, which reads it
 * back as drawn; and the costs, the table's against lib/gfx's scalar
 * entries, linked in. Exits 0, or the step that failed; the second
 * exits 0, or its own step.
 */
#include <planar/gfx.h>
#include <planar/msg.h>

#include "gfxcheck.h"
#include "log.h"

#define CHECK(step, cond) \
    if (!(cond)) \
        return (step)

constexpr uint64_t SECOND = 1;
constexpr int64_t SIG_PORT = 20, SIG_CONN = 21, SIG_DEAD = 22;

/* What the second asks for, and is told. */
struct ShareMsg {
    struct Message sm_Msg;
    Handle sm_Task;                     /* the second's, to share into */
    uint32_t sm_Crc;
    uint64_t sm_Base;
    uint32_t sm_Width, sm_Height, sm_BytesPerRow, sm_Pad;
};

static struct GfxIFace *IGfx;

static uint32_t crc_of(const void *base, uint32_t w, uint32_t h, uint32_t bpr)
{
    uint32_t crc = ~0u;

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = ((const uint32_t *)((const uint8_t *)base + (uint64_t)y * bpr))[x];

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

static int open_gfx(void)
{
    struct Library *lib;

    CHECK(1, (lib = IExec->OpenLibrary("gfx.library", 7)));
    CHECK(2, lib->lib_Version == 8 && !IExec->OpenLibrary("gfx.library", 9));
    IGfx = (struct GfxIFace *)IExec->GetInterface(lib, "main", 1);
    CHECK(3, IGfx && IGfx->Data.if_Library == lib && IGfx->Data.if_Size == sizeof(struct GfxIFace));
    CHECK(4, !IExec->GetInterface(lib, "main", 2));
    return 0;
}

/* The second process: connect, be shared the bitmap, and read it. */
static int second(void)
{
    struct MsgPort mp;
    struct ShareMsg *m;
    struct AreaInfo ai;
    const struct BitMap *bm;

    CHECK(50, ConnectPort(&mp, "gfx.share", SIG_CONN, 2, 2 * sizeof *m) == 0);
    CHECK(51, (m = AllocMsg(&mp, sizeof *m)));
    *m = (struct ShareMsg){ .sm_Msg.mn_Length = sizeof *m, .sm_Task = FindTask(nullptr) };
    CHECK(52, DoMsg(&mp, &m->sm_Msg) == 0 && m->sm_Msg.mn_Error == 0);
    CHECK(53, QueryArea((void *)(uintptr_t)m->sm_Base, &ai) == 0 && ai.ai_Rights == AREAR_READ &&
                  ai.ai_Sharers == 2);
    CHECK(54, crc_of((void *)(uintptr_t)m->sm_Base, m->sm_Width, m->sm_Height, m->sm_BytesPerRow) == m->sm_Crc);
    /* Its BitMap is in the same Area, after the pixels. */
    bm = (const struct BitMap *)(m->sm_Base + (uint64_t)m->sm_BytesPerRow * m->sm_Height);
    CHECK(55, bm->bm_BaseAddr == (void *)(uintptr_t)m->sm_Base && bm->bm_Width == m->sm_Width);
    ClosePort(&mp);
    return 0;
}

static int share(void)
{
    struct BitMap *bm;
    struct RastPort rp;
    struct MsgPort mp;
    struct ShareMsg *m;
    struct ProcessInfo pi;
    int64_t port, h;

    CHECK(30, (bm = IGfx->AllocBitMap(64, 48, PIXFMT_ARGB8888, 0, nullptr)));
    IGfx->InitRastPort(&rp, bm);
    rp.rp_FgPen = 0x80c08040u;
    IGfx->RectFill(&rp, 0, 0, 63, 47);
    IGfx->DrawBevel(&rp, 2, 2, 61, 45, 0xffffffffu, 0xff404040u);
    rp.rp_FgPen = 0xff000000u;
    rp.rp_DrawMode = JAM1;
    IGfx->Text(&rp, 8, 20, "shared", 6);

    CHECK(31, (port = CreatePort("gfx.share", SIG_PORT)) > 0);
    CHECK(32, (h = CreateProcess("gfx", SECOND, SIG_DEAD, 0)) > 0);
    while (AcceptPort(&mp, (Handle)port, SIG_CONN) != 0)
        Wait(1ULL << SIG_PORT);
    while (!(m = (struct ShareMsg *)GetMsgSized(&mp, sizeof *m))) {
        CHECK(33, !PortGone(&mp));
        if (ArmPort(&mp))
            Wait(1ULL << SIG_CONN);
    }
    CHECK(34, IExec->ShareArea(bm->bm_Area, m->sm_Task, AREAR_READ) == 0);
    m->sm_Base = (uint64_t)(uintptr_t)bm->bm_BaseAddr;
    m->sm_Width = bm->bm_Width;
    m->sm_Height = bm->bm_Height;
    m->sm_BytesPerRow = bm->bm_BytesPerRow;
    m->sm_Crc = crc_of(bm->bm_BaseAddr, bm->bm_Width, bm->bm_Height, bm->bm_BytesPerRow);
    m->sm_Msg.mn_Error = 0;
    CHECK(35, ReplyMsg(&mp, &m->sm_Msg) == 0);
    Wait(1ULL << SIG_DEAD);
    CHECK(36, QueryProcess((Handle)h, &pi) == 0 && pi.pi_State == PROCS_EXITED);
    CHECK(37, pi.pi_ExitCode == 0);
    CloseHandle((Handle)h);
    ClosePort(&mp);
    DeletePort((Handle)port);
    IGfx->FreeBitMap(bm);
    return 0;
}

/* Microseconds a megapixel: the table's entry against lib/gfx's own. */
static int costs(void)
{
    constexpr uint32_t N = 512, REPS = 4;
    struct BitMap *dst, *src;
    struct RastPort rp;
    uint64_t t[5];
    char line[N / 8];

    CHECK(40, (dst = IGfx->AllocBitMap(N, N, PIXFMT_XRGB8888, 0, nullptr)) &&
                  (src = IGfx->AllocBitMap(N, N, PIXFMT_ARGB8888, 0, nullptr)));
    IGfx->InitRastPort(&rp, src);
    for (uint32_t y = 0; y < N; y += 16) {
        rp.rp_FgPen = (y * 2) << 24 | y * 0x10307u;
        IGfx->RectFill(&rp, 0, (int32_t)y, N - 1, (int32_t)y + 15);
    }
    IGfx->InitRastPort(&rp, dst);
    IGfx->RectFill(&rp, 0, 0, N - 1, N - 1);            /* the vector unit's state, made */
    t[0] = CurrentTime();
    for (uint32_t i = 0; i < REPS; i++)
        RectFill(&rp, 0, 0, N - 1, N - 1);
    t[1] = CurrentTime();
    for (uint32_t i = 0; i < REPS; i++)
        IGfx->RectFill(&rp, 0, 0, N - 1, N - 1);
    t[2] = CurrentTime();
    for (uint32_t i = 0; i < REPS; i++)
        BltBitMap(src, 0, 0, &rp, 0, 0, N, N, BLEND_SRC_OVER);
    t[3] = CurrentTime();
    for (uint32_t i = 0; i < REPS; i++)
        IGfx->BltBitMap(src, 0, 0, &rp, 0, 0, N, N, BLEND_SRC_OVER);
    t[4] = CurrentTime();
    for (unsigned i = 0; i < sizeof line; i++)
        line[i] = (char)(' ' + i % 95);
    for (unsigned k = 0; k < 4; k++)
        t[k] = (t[k + 1] - t[k]) / REPS * 1000000 / (N * N) / 1000;
    t[4] = CurrentTime();
    for (uint32_t y = 0; y < N; y += 8)
        IGfx->Text(&rp, 0, (int32_t)y, line, N / 8);
    t[4] = (CurrentTime() - t[4]) / (N / 8 * (N / 8));
    KPrintF("gfx: a megapixel's RectFill %lu us scalar, %lu us by the table; SRC_OVER %lu us, %lu us; "
            "Text %lu ns a glyph", t[0], t[1], t[2], t[3], t[4]);
    IGfx->FreeBitMap(src);
    IGfx->FreeBitMap(dst);
    return 0;
}

int main(uint64_t arg)
{
    unsigned cases = 0, line;
    int r;

    if (arg == SECOND)
        return second();
    if ((r = open_gfx()))
        return r;
    if ((line = gc_all(IGfx, &cases))) {
        KPrintF("gfx: gfxcheck.h:%u failed (width %d, at %d, place %d)", line, gc_w, gc_a, gc_p);
        return 10;
    }
    if ((r = share()) || (r = costs()))
        return r;
    KPrintF("gfx: %u cases through IGfx drawn as the reference draws them; a bitmap shared "
            "read-only with a second process, read back as drawn", cases);
    return 0;
}
