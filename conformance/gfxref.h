/*
 * conformance/gfxref.h -- gfx.library's operations, drawn by a reference
 *
 * A pixel at a time, each clipped on its own, and blended by round(x /
 * 255) computed its own way: nothing of lib/gfx's. As an interface too
 * (gr_iface), for code that draws the same thing both ways: gfxcheck.h's
 * cases, and displaytest's overlay, read back from the screen.
 */
#ifndef CONFORMANCE_GFXREF_H
#define CONFORMANCE_GFXREF_H

#include <planar/gfx.h>

#include "../lib/gfx/font8x8.h"

[[maybe_unused]] static bool gr_in(const struct RastPort *rp, int64_t x, int64_t y)
{
    const struct BitMap *bm = rp->rp_BitMap;

    return x >= 0 && y >= 0 && x < bm->bm_Width && y < bm->bm_Height && x >= rp->rp_Clip.MinX &&
           x <= rp->rp_Clip.MaxX && y >= rp->rp_Clip.MinY && y <= rp->rp_Clip.MaxY;
}

[[maybe_unused]] static uint32_t *gr_px(const struct BitMap *bm, int64_t x, int64_t y)
{
    return (uint32_t *)((uint8_t *)bm->bm_BaseAddr + y * bm->bm_BytesPerRow) + x;
}

[[maybe_unused]] static void gr_fill(struct RastPort *rp, int64_t x0, int64_t y0, int64_t x1,
                                     int64_t y1, uint32_t pen)
{
    for (int64_t y = y0; y <= y1; y++)
        for (int64_t x = x0; x <= x1; x++)
            if (gr_in(rp, x, y))
                *gr_px(rp->rp_BitMap, x, y) = pen;
}

[[maybe_unused]] static void gr_bevel(struct RastPort *rp, int64_t x0, int64_t y0, int64_t x1,
                                      int64_t y1, uint32_t light, uint32_t dark)
{
    if (x0 > x1 || y0 > y1)
        return;
    gr_fill(rp, x0, y0, x1, y0, light);
    gr_fill(rp, x0, y0, x0, y1, light);
    gr_fill(rp, x0, y1, x1, y1, dark);
    gr_fill(rp, x1, y0, x1, y1, dark);
}

/* s over d, alpha blended as a channel whose source is 255. */
[[maybe_unused]] static uint32_t gr_over(uint32_t s, uint32_t d)
{
    uint32_t a = s >> 24, r = 0;

    for (unsigned k = 0; k < 4; k++) {
        uint32_t sc = k == 3 ? 255 : s >> 8 * k & 0xff, dc = d >> 8 * k & 0xff;
        uint32_t x = sc * a + dc * (255 - a);

        r |= (2 * x + 255) / 510 << 8 * k;     /* x / 255, to the nearest: never a half */
    }
    return r;
}

/* Every source pixel read before any is written, as memmove does. */
[[maybe_unused]] static void gr_blt(const struct BitMap *src, int64_t sx, int64_t sy,
                                    struct RastPort *dst, int64_t dx, int64_t dy, int64_t w,
                                    int64_t h, uint32_t blend)
{
    constexpr int64_t MAX = 4096;
    static uint32_t px[MAX];
    static bool ok[MAX];

    if ((blend != BLEND_SRC && blend != BLEND_SRC_OVER) || w <= 0 || h <= 0 || w * h > MAX)
        return;
    for (int64_t j = 0; j < h; j++)
        for (int64_t i = 0; i < w; i++) {
            int64_t x = sx + i, y = sy + j;

            ok[j * w + i] = x >= 0 && y >= 0 && x < src->bm_Width && y < src->bm_Height;
            if (ok[j * w + i])
                px[j * w + i] = *gr_px(src, x, y);
        }
    for (int64_t j = 0; j < h; j++)
        for (int64_t i = 0; i < w; i++) {
            uint32_t *d;

            if (!ok[j * w + i] || !gr_in(dst, dx + i, dy + j))
                continue;
            d = gr_px(dst->rp_BitMap, dx + i, dy + j);
            *d = blend == BLEND_SRC_OVER && src->bm_PixelFormat == PIXFMT_ARGB8888
                     ? gr_over(px[j * w + i], *d) : px[j * w + i];
        }
}

[[maybe_unused]] static void gr_text(struct RastPort *rp, int64_t x, int64_t y, const char *s,
                                     uint64_t n)
{
    static const struct TextFont eight = { 8, 8, FONT8X8_FIRST, FONT8X8_FIRST + FONT8X8_COUNT - 1,
                                           font8x8[0] };
    const struct TextFont *f = rp->rp_Font ? rp->rp_Font : &eight;

    if (f->tf_XSize > 8)
        return;
    for (uint64_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i];
        unsigned g = c >= f->tf_LoChar && c <= f->tf_HiChar ? c - f->tf_LoChar : 0;

        for (int64_t r = 0; r < f->tf_YSize; r++)
            for (int64_t b = 0; b < f->tf_XSize; b++) {
                int64_t px = x + (int64_t)i * f->tf_XSize + b, py = y + r;
                bool on = f->tf_CharData[g * f->tf_YSize + r] >> b & 1;

                if (gr_in(rp, px, py) && (on || rp->rp_DrawMode == JAM2))
                    *gr_px(rp->rp_BitMap, px, py) = on ? rp->rp_FgPen : rp->rp_BgPen;
            }
    }
}

/* The interface's drawing calls by the reference; the rest nullptr. */
[[maybe_unused]] static void gr_RectFill(struct RastPort *rp, int32_t x0, int32_t y0, int32_t x1,
                                         int32_t y1)
{
    gr_fill(rp, x0, y0, x1, y1, rp->rp_FgPen);
}

[[maybe_unused]] static void gr_BltBitMap(const struct BitMap *src, int32_t sx, int32_t sy,
                                          struct RastPort *dst, int32_t dx, int32_t dy, int32_t w,
                                          int32_t h, uint32_t blend)
{
    gr_blt(src, sx, sy, dst, dx, dy, w, h, blend);
}

[[maybe_unused]] static void gr_DrawBevel(struct RastPort *rp, int32_t x0, int32_t y0, int32_t x1,
                                          int32_t y1, uint32_t light, uint32_t dark)
{
    gr_bevel(rp, x0, y0, x1, y1, light, dark);
}

[[maybe_unused]] static void gr_Text(struct RastPort *rp, int32_t x, int32_t y, const char *s,
                                     uint64_t n)
{
    if (s)
        gr_text(rp, x, y, s, n);
}

[[maybe_unused]] static const struct GfxIFace gr_iface = {
    .RectFill = gr_RectFill, .BltBitMap = gr_BltBitMap, .DrawBevel = gr_DrawBevel, .Text = gr_Text,
};

#endif /* CONFORMANCE_GFXREF_H */
