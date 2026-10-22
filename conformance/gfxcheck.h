/*
 * conformance/gfxcheck.h -- gfx.library against a reference, to the bit
 *
 * Each case starts two bitmaps from the same random pixels, draws in one
 * through the interface under test and in the other by gfxref.h's
 * reference, and compares the whole of both buffers, the guard columns
 * and rows around each bitmap included. RectFill, DrawBevel and BltBitMap (both blends,
 * and from an XRGB source) at every width from 0 to 130 and 8
 * alignments, in five places: inside, off the left, right, top and
 * bottom edges, and inside a smaller clip. Then blits within one bitmap,
 * overlapping each way; Text, its clipping, JAM1 and JAM2 and another
 * font; and the bitmap calls. Shared by conformance/gfx.c, in the guest
 * through IGfx (by the vector unit there), and lib/gfx/test.c, on the
 * host with the sanitizers, through a scalar table and a NEON one.
 */
#ifndef CONFORMANCE_GFXCHECK_H
#define CONFORMANCE_GFXCHECK_H

#include <planar/gfx.h>
#include <planar/syscalls.h>

#include "gfxref.h"

#define GC(cond) \
    do { \
        if (!(cond)) \
            return __LINE__; \
        (*cases)++; \
    } while (0)

constexpr int32_t GC_W = 144, GC_H = 10;           /* each bitmap */
constexpr int32_t GC_STRIDE = 160, GC_ROWS = GC_H + 2;  /* its buffer: guards around it */
constexpr unsigned GC_PIXELS = GC_STRIDE * GC_ROWS;

/* Where the last case was, for the host's message. */
static int gc_w, gc_a, gc_p;

static uint32_t gc_buf[2][GC_PIXELS], gc_pattern[GC_PIXELS], gc_srcbuf[GC_PIXELS];
static struct BitMap gc_bm[2], gc_src, gc_xsrc;
static struct RastPort gc_rp[2];
static uint64_t gc_seed = 0x9e3779b97f4a7c15u;

static uint32_t gc_rand(void)
{
    gc_seed ^= gc_seed << 13;
    gc_seed ^= gc_seed >> 7;
    gc_seed ^= gc_seed << 17;
    return (uint32_t)(gc_seed >> 16);
}

/* Both bitmaps the pattern again, and their whole buffers' clip. */
static void gc_reset(void)
{
    for (unsigned i = 0; i < GC_PIXELS; i++)
        gc_buf[0][i] = gc_buf[1][i] = gc_pattern[i];
    for (unsigned k = 0; k < 2; k++)
        gc_rp[k].rp_Clip = (struct GfxRect){ 0, 0, GC_W - 1, GC_H - 1 };
}

static bool gc_same(void)
{
    for (unsigned i = 0; i < GC_PIXELS; i++)
        if (gc_buf[0][i] != gc_buf[1][i])
            return false;
    return true;
}

static void gc_setup(void)
{
    for (unsigned i = 0; i < GC_PIXELS; i++) {
        uint32_t a = (uint32_t[]){ 0, 255, 128, 1, 254 }[i % 5];

        gc_pattern[i] = gc_rand();
        /* Every alpha, and the ends often. */
        gc_srcbuf[i] = (i % 7 < 5 ? a : gc_rand() & 0xff) << 24 | (gc_rand() & 0xffffff);
    }
    for (unsigned k = 0; k < 2; k++) {
        InitBitMap(&gc_bm[k], gc_buf[k] + GC_STRIDE + 4, GC_W, GC_H, GC_STRIDE * 4, PIXFMT_XRGB8888);
        gc_rp[k] = (struct RastPort){ .rp_BitMap = &gc_bm[k], .rp_DrawMode = JAM2 };
    }
    InitBitMap(&gc_src, gc_srcbuf + GC_STRIDE + 4, GC_W, GC_H, GC_STRIDE * 4, PIXFMT_ARGB8888);
    InitBitMap(&gc_xsrc, gc_srcbuf + GC_STRIDE + 4, GC_W, GC_H, GC_STRIDE * 4, PIXFMT_XRGB8888);
    gc_reset();
}

/* Place p of a rectangle w wide at alignment a: its corners, and the
 * clip it's drawn in. */
static void gc_place(int32_t w, int32_t a, int p, int32_t r[4])
{
    switch (p) {
    case 0: r[0] = a, r[1] = 1, r[3] = 3; break;
    case 1: r[0] = a - 12, r[1] = 2, r[3] = 4; break;                   /* off the left */
    case 2: r[0] = GC_W - w + 8 - a, r[1] = 5, r[3] = 7; break;         /* off the right */
    case 3: r[0] = a, r[1] = -3, r[3] = GC_H + 2; break;               /* off the top and bottom */
    default: r[0] = a, r[1] = 0, r[3] = GC_H - 1; break;
    }
    r[2] = r[0] + w - 1;
    for (unsigned k = 0; k < 2; k++)
        gc_rp[k].rp_Clip = p == 4 ? (struct GfxRect){ a + 3, 2, a + w / 2, GC_H - 3 }
                         : p == 2 ? (struct GfxRect){ -1000, -1000, 1000, 1000 }
                                  : (struct GfxRect){ 0, 0, GC_W - 1, GC_H - 1 };
}

static unsigned gc_shapes(const struct GfxIFace *g, unsigned *cases)
{
    for (int32_t w = 0; w <= 130; w++)
        for (int32_t a = 0; a < 8; a++)
            for (int p = 0; p < 5; p++) {
                uint32_t pen = gc_rand(), dark = gc_rand();
                int32_t r[4], sx = p == 1 ? -a - 1 : p == 2 ? GC_W - w / 2 : (a * 5) & 7;
                int32_t sy = p == 3 ? -1 : 1;

                gc_w = w, gc_a = a, gc_p = p;
                gc_reset();
                gc_place(w, a, p, r);
                gc_rp[0].rp_FgPen = gc_rp[1].rp_FgPen = pen;
                g->RectFill(&gc_rp[0], r[0], r[1], r[2], r[3]);
                gr_fill(&gc_rp[1], r[0], r[1], r[2], r[3], pen);
                GC(gc_same());
                g->DrawBevel(&gc_rp[0], r[0], r[1], r[2], r[3], pen, dark);
                gr_bevel(&gc_rp[1], r[0], r[1], r[2], r[3], pen, dark);
                GC(gc_same());
                for (uint32_t blend = BLEND_SRC; blend <= BLEND_SRC_OVER; blend++) {
                    g->BltBitMap(&gc_src, sx, sy, &gc_rp[0], r[0], r[1], w, r[3] - r[1] + 1, blend);
                    gr_blt(&gc_src, sx, sy, &gc_rp[1], r[0], r[1], w, r[3] - r[1] + 1, blend);
                    GC(gc_same());
                }
                g->BltBitMap(&gc_xsrc, sx, sy, &gc_rp[0], r[0], r[1], w, r[3] - r[1] + 1,
                             BLEND_SRC_OVER);
                gr_blt(&gc_xsrc, sx, sy, &gc_rp[1], r[0], r[1], w, r[3] - r[1] + 1, BLEND_SRC_OVER);
                GC(gc_same());
            }
    return 0;
}

/* Within one bitmap, the destination before, after, above and below
 * its source, by up to 9 pixels and a row. */
static unsigned gc_overlaps(const struct GfxIFace *g, unsigned *cases)
{
    static const int32_t widths[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 64, 100, 120 };

    for (unsigned wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
        for (int32_t ddx = -9; ddx <= 9; ddx++)
            for (int32_t ddy = -1; ddy <= 1; ddy++)
                for (uint32_t blend = BLEND_SRC; blend <= BLEND_SRC_OVER; blend++) {
                    int32_t w = widths[wi];

                    gc_w = w, gc_a = ddx, gc_p = 10 + ddy;
                    gc_reset();
                    gc_bm[0].bm_PixelFormat = gc_bm[1].bm_PixelFormat = PIXFMT_ARGB8888;
                    g->BltBitMap(&gc_bm[0], 11, 4, &gc_rp[0], 11 + ddx, 4 + ddy, w, 4, blend);
                    gr_blt(&gc_bm[1], 11, 4, &gc_rp[1], 11 + ddx, 4 + ddy, w, 4, blend);
                    gc_bm[0].bm_PixelFormat = gc_bm[1].bm_PixelFormat = PIXFMT_XRGB8888;
                    GC(gc_same());
                }
    /* An unknown blend draws nothing. */
    gc_reset();
    g->BltBitMap(&gc_src, 0, 0, &gc_rp[0], 0, 0, GC_W, GC_H, 7);
    GC(gc_same());
    return 0;
}

static unsigned gc_text(const struct GfxIFace *g, unsigned *cases)
{
    static const char s[] = "Planar 7, \x01\x7f\xff~ at last";
    static const uint8_t abc[] = { 0x1f, 0x11, 0x1f, 0x0a, 0x15, 0x0a, 0x04, 0x0e, 0x1f };
    static const struct TextFont small = { 3, 5, 'A', 'C', abc }, wide = { 8, 9, 'A', 'C', abc };
    static const int32_t xs[] = { 0, 3, -9, -1, GC_W - 20, GC_W - 3 }, ys[] = { 1, -3, -7, GC_H - 5 };

    for (unsigned n = 0; n < sizeof s; n++)
        for (unsigned xi = 0; xi < sizeof xs / sizeof xs[0]; xi++)
            for (unsigned yi = 0; yi < sizeof ys / sizeof ys[0]; yi++)
                for (uint32_t mode = JAM1; mode <= JAM2; mode++)
                    for (unsigned f = 0; f < 3; f++) {
                        gc_w = (int)n, gc_a = xs[xi], gc_p = 20 + ys[yi];
                        gc_reset();
                        for (unsigned k = 0; k < 2; k++)
                            gc_rp[k] = (struct RastPort){
                                .rp_BitMap = &gc_bm[k],
                                .rp_Clip = f == 2 ? (struct GfxRect){ 2, 1, GC_W - 8, GC_H - 2 }
                                                  : (struct GfxRect){ 0, 0, GC_W - 1, GC_H - 1 },
                                .rp_FgPen = 0x11223344u + n, .rp_BgPen = 0x55667788u ^ n,
                                .rp_DrawMode = mode,
                                .rp_Font = f == 1 ? &small : nullptr,
                            };
                        g->Text(&gc_rp[0], xs[xi], ys[yi], s, n);
                        gr_text(&gc_rp[1], xs[xi], ys[yi], s, n);
                        GC(gc_same());
                    }
    /* A font wider than a byte draws nothing; nor does no string. */
    gc_reset();
    gc_rp[0].rp_Font = &wide;
    g->Text(&gc_rp[0], 0, 0, "ABC", 3);
    gc_rp[0].rp_Font = nullptr;
    g->Text(&gc_rp[0], 0, 0, nullptr, 3);
    GC(gc_same());
    gc_rp[0].rp_DrawMode = gc_rp[1].rp_DrawMode = JAM2;
    return 0;
}

static unsigned gc_bitmaps(const struct GfxIFace *g, unsigned *cases)
{
    static uint32_t row[512];
    struct BitMap *bm, *f, *b2, ib, fr;
    struct RastPort rp;
    struct AreaInfo ai;
    void *nc;
    int64_t h;

    /* Refused. */
    GC(!g->AllocBitMap(0, 4, PIXFMT_XRGB8888, 0, nullptr) &&
       !g->AllocBitMap(4, 0, PIXFMT_XRGB8888, 0, nullptr));
    GC(!g->AllocBitMap(GFX_MAX_SIZE + 1, 1, PIXFMT_XRGB8888, 0, nullptr));
    GC(!g->AllocBitMap(4, 4, 3, 0, nullptr) && !g->AllocBitMap(4, 4, PIXFMT_XRGB8888, 1u << 5, nullptr));

    /* One, and what it says of itself. */
    GC((bm = g->AllocBitMap(100, 7, PIXFMT_ARGB8888, BMF_CLEAR, nullptr)));
    GC(bm->bm_Area && bm->bm_BaseAddr && ((uintptr_t)bm->bm_BaseAddr & 63) == 0);
    GC(bm->bm_Width == 100 && bm->bm_Height == 7 && bm->bm_BytesPerRow == 448);
    GC((uint8_t *)bm == (uint8_t *)bm->bm_BaseAddr + 448 * 7);     /* after its last row */
    GC(g->GetBitMapAttr(bm, BMA_WIDTH) == 100 && g->GetBitMapAttr(bm, BMA_HEIGHT) == 7);
    GC(g->GetBitMapAttr(bm, BMA_DEPTH) == 32 && g->GetBitMapAttr(bm, BMA_BYTESPERROW) == 448);
    GC(g->GetBitMapAttr(bm, BMA_PIXELFORMAT) == PIXFMT_ARGB8888 &&
       g->GetBitMapAttr(bm, BMA_MODIFIER) == MOD_LINEAR);
    GC(g->GetBitMapAttr(bm, BMA_FLAGS) == BMF_CLEAR && g->GetBitMapAttr(bm, BMA_COLORSPACE) == CS_SRGB);
    GC(g->GetBitMapAttr(bm, BMA_BASEADDR) == (uint64_t)(uintptr_t)bm->bm_BaseAddr &&
       g->GetBitMapAttr(bm, BMA_AREA) == bm->bm_Area);
    GC(g->GetBitMapAttr(bm, 99) == 0 && g->GetBitMapAttr(nullptr, BMA_WIDTH) == 0);
    for (unsigned y = 0; y < 7; y++)
        for (unsigned x = 0; x < 100; x++)
            GC(*gr_px(bm, x, y) == 0);
    g->InitRastPort(&rp, bm);
    GC(rp.rp_BitMap == bm && rp.rp_Clip.MinX == 0 && rp.rp_Clip.MinY == 0 &&
       rp.rp_Clip.MaxX == 99 && rp.rp_Clip.MaxY == 6);
    GC(rp.rp_FgPen == 0xffffffffu && rp.rp_BgPen == 0xff000000u && rp.rp_DrawMode == JAM2 &&
       !rp.rp_Font);
    rp.rp_FgPen = 0x80402010u;
    g->RectFill(&rp, -5, -5, 200, 200);
    GC(*gr_px(bm, 0, 0) == 0x80402010u && *gr_px(bm, 99, 6) == 0x80402010u && bm->bm_Width == 100);
    g->FreeBitMap(bm);
    g->FreeBitMap(nullptr);

    /* A friend's stride, when as wide; its placement. */
    GC((f = g->AllocBitMap(256, 4, PIXFMT_XRGB8888, 0, nullptr)) && f->bm_BytesPerRow == 1024);
    GC(g->InitBitMap(&fr, row, 256, 1, 2048, PIXFMT_XRGB8888));
    GC((b2 = g->AllocBitMap(256, 2, PIXFMT_ARGB8888, BMF_DISPLAYABLE, &fr)));
    GC(b2->bm_BytesPerRow == 2048 && b2->bm_Flags == (BMF_DISPLAYABLE | BMF_CLEAR));
    g->FreeBitMap(b2);
    GC((b2 = g->AllocBitMap(256, 2, PIXFMT_ARGB8888, 0, &fr)) && b2->bm_BytesPerRow == 1024);
    g->FreeBitMap(b2);
    GC((b2 = g->AllocBitMap(100, 2, PIXFMT_ARGB8888, BMF_DISPLAYABLE, &fr)) &&
       b2->bm_BytesPerRow == 448);
    g->FreeBitMap(b2);
    if ((h = AllocArea(8192, AREAF_NC, &nc)) > 0) {         /* not where there's no Svpbmt */
        GC(g->InitBitMap(&fr, nc, 256, 2, 1024, PIXFMT_XRGB8888));
        GC((b2 = g->AllocBitMap(256, 2, PIXFMT_XRGB8888, BMF_DISPLAYABLE, &fr)));
        GC(QueryArea(b2->bm_BaseAddr, &ai) == 0 && (ai.ai_Flags & AREAF_NC));
        g->FreeBitMap(b2);
        GC((b2 = g->AllocBitMap(256, 2, PIXFMT_XRGB8888, 0, &fr)));
        GC(QueryArea(b2->bm_BaseAddr, &ai) == 0 && !(ai.ai_Flags & AREAF_NC));
        g->FreeBitMap(b2);
        FreeArea((Handle)h);
    }
    g->FreeBitMap(f);

    /* The caller's memory. */
    GC(!g->InitBitMap(&ib, row, 100, 2, 396, PIXFMT_XRGB8888));     /* rows too short */
    GC(!g->InitBitMap(&ib, (uint8_t *)row + 2, 10, 2, 64, PIXFMT_XRGB8888));
    GC(!g->InitBitMap(&ib, row, 10, 2, 64, 0) && !g->InitBitMap(&ib, nullptr, 10, 2, 64, 1));
    GC(!g->InitBitMap(&ib, row, 10, 2, 42, PIXFMT_XRGB8888));
    GC(g->InitBitMap(&ib, row, 10, 2, 40, PIXFMT_ARGB8888) && ib.bm_Area == 0 &&
       ib.bm_BaseAddr == row && ib.bm_BytesPerRow == 40 && ib.bm_Flags == 0);
    g->FreeBitMap(&ib);                                     /* nothing to free */
    return 0;
}

/* Every case through 'g': 0, or the line of the check that failed. */
static unsigned gc_all(const struct GfxIFace *g, unsigned *cases)
{
    unsigned r;

    gc_setup();
    if ((r = gc_shapes(g, cases)) || (r = gc_overlaps(g, cases)) || (r = gc_text(g, cases)) ||
        (r = gc_bitmaps(g, cases)))
        return r;
    return 0;
}

#endif /* CONFORMANCE_GFXCHECK_H */
