/*
 * lib/gfx/draw.c -- RectFill, BltBitMap and DrawBevel: clipping, and the
 * scalar row kernels (V7 step 2; mask and sweep, V8 step 3)
 *
 * Each operation clips to its rastport, then hands a kernel one row at a
 * time. The scalar kernels are plain loops the compiler is told not to
 * vectorise: they're the reference the vector unit's are tested against
 * (lib/gfx/test.c, conformance/gfx.c), and what a CPU without one runs.
 */
#include "rows.h"

static void fill_scalar(uint32_t *dst, uint32_t pixel, uint64_t n)
{
#pragma clang loop vectorize(disable) interleave(disable)
    for (uint64_t i = 0; i < n; i++)
        dst[i] = pixel;
}

static void copy_scalar(uint32_t *dst, const uint32_t *src, uint64_t n)
{
#pragma clang loop vectorize(disable) interleave(disable)
    for (uint64_t i = 0; i < n; i++)
        dst[i] = src[i];
}

static void over_scalar(uint32_t *dst, const uint32_t *src, uint64_t n)
{
#pragma clang loop vectorize(disable) interleave(disable)
    for (uint64_t i = 0; i < n; i++)
        dst[i] = gfx_over(src[i], dst[i]);
}

static void mask_scalar(uint32_t *dst, const uint8_t *cov, const uint32_t *src, uint64_t n)
{
#pragma clang loop vectorize(disable) interleave(disable)
    for (uint64_t i = 0; i < n; i++)
        dst[i] = gfx_mask_px(src[i], dst[i], cov[i]);
}

static void sweep_scalar(uint8_t *out, const int32_t *area, uint64_t n, bool evenodd)
{
    uint32_t sum = 0;

#pragma clang loop vectorize(disable) interleave(disable)
    for (uint64_t i = 0; i < n; i++) {
        sum += (uint32_t)area[i];
        out[i] = gfx_sweep_px(sum, evenodd);
    }
}

const struct gfx_rows gfx_rows_scalar = { fill_scalar, copy_scalar, over_scalar, mask_scalar,
                                          sweep_scalar };

void gfx_rect_fill_pen(const struct gfx_rows *k, struct RastPort *rp, int64_t x0, int64_t y0,
                       int64_t x1, int64_t y1, uint32_t pen)
{
    int64_t cx0, cy0, cx1, cy1;

    if (!gfx_clip_of(rp, &cx0, &cy0, &cx1, &cy1))
        return;
    if (x0 < cx0)
        x0 = cx0;
    if (y0 < cy0)
        y0 = cy0;
    if (x1 > cx1)
        x1 = cx1;
    if (y1 > cy1)
        y1 = cy1;
    for (int64_t y = y0; x0 <= x1 && y <= y1; y++)
        k->fill(gfx_pixel_at(rp->rp_BitMap, x0, y), pen, (uint64_t)(x1 - x0 + 1));
}

void gfx_rect_fill(const struct gfx_rows *k, struct RastPort *rp, int32_t x0, int32_t y0,
                   int32_t x1, int32_t y1)
{
    if (rp)
        gfx_rect_fill_pen(k, rp, x0, y0, x1, y1, rp->rp_FgPen);
}

/* The light's from the top left (docs §13.2): its edges first, then the
 * shadow's, which so has the two corners they share. */
void gfx_bevel(const struct gfx_rows *k, struct RastPort *rp, int32_t x0, int32_t y0,
               int32_t x1, int32_t y1, uint32_t light, uint32_t dark)
{
    if (x0 > x1 || y0 > y1)
        return;
    gfx_rect_fill_pen(k, rp, x0, y0, x1, y0, light);
    gfx_rect_fill_pen(k, rp, x0, y0, x0, y1, light);
    gfx_rect_fill_pen(k, rp, x0, y1, x1, y1, dark);
    gfx_rect_fill_pen(k, rp, x1, y0, x1, y1, dark);
}

/* One row of a blit. A destination after its source in the same row is
 * done from the right, in pieces no longer than the distance between
 * them, so no piece reads what another has written. */
static void blt_row(void (*op)(uint32_t *, const uint32_t *, uint64_t), uint32_t *d,
                    const uint32_t *s, uint64_t n)
{
    uint64_t gap;

    if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n * 4) {
        op(d, s, n);
        return;
    }
    gap = ((uintptr_t)d - (uintptr_t)s) / 4;
    while (n) {
        uint64_t piece = n < gap ? n : gap;

        n -= piece;
        op(d + n, s + n, piece);
    }
}

void gfx_blt(const struct gfx_rows *k, const struct BitMap *src, int32_t sx32, int32_t sy32,
             struct RastPort *dst, int32_t dx32, int32_t dy32, int32_t w32, int32_t h32,
             uint32_t blend)
{
    int64_t sx = sx32, sy = sy32, dx = dx32, dy = dy32, w = w32, h = h32;
    int64_t cx0, cy0, cx1, cy1;
    void (*op)(uint32_t *, const uint32_t *, uint64_t);

    if (!src || !src->bm_BaseAddr || !gfx_clip_of(dst, &cx0, &cy0, &cx1, &cy1))
        return;
    if (blend == BLEND_SRC)
        op = k->copy;
    else if (blend == BLEND_SRC_OVER)
        op = src->bm_PixelFormat == PIXFMT_ARGB8888 ? k->over : k->copy;
    else
        return;
    /* To the source, then to the clip, moving both corners together. */
    if (sx < 0)
        dx -= sx, w += sx, sx = 0;
    if (sy < 0)
        dy -= sy, h += sy, sy = 0;
    if (sx + w > src->bm_Width)
        w = src->bm_Width - sx;
    if (sy + h > src->bm_Height)
        h = src->bm_Height - sy;
    if (dx < cx0)
        sx += cx0 - dx, w -= cx0 - dx, dx = cx0;
    if (dy < cy0)
        sy += cy0 - dy, h -= cy0 - dy, dy = cy0;
    if (dx + w - 1 > cx1)
        w = cx1 - dx + 1;
    if (dy + h - 1 > cy1)
        h = cy1 - dy + 1;
    if (w <= 0 || h <= 0)
        return;
    /* Rows bottom up when the destination is after the source. */
    if ((uintptr_t)gfx_pixel_at(dst->rp_BitMap, dx, dy) > (uintptr_t)gfx_pixel_at(src, sx, sy))
        for (int64_t y = h - 1; y >= 0; y--)
            blt_row(op, gfx_pixel_at(dst->rp_BitMap, dx, dy + y), gfx_pixel_at(src, sx, sy + y), (uint64_t)w);
    else
        for (int64_t y = 0; y < h; y++)
            blt_row(op, gfx_pixel_at(dst->rp_BitMap, dx, dy + y), gfx_pixel_at(src, sx, sy + y), (uint64_t)w);
}

void RectFill(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax)
{
    gfx_rect_fill(&gfx_rows_scalar, rp, xMin, yMin, xMax, yMax);
}

void BltBitMap(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
               int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend)
{
    gfx_blt(&gfx_rows_scalar, src, xSrc, ySrc, dst, xDest, yDest, xSize, ySize, blend);
}

void DrawBevel(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
               uint32_t light, uint32_t dark)
{
    gfx_bevel(&gfx_rows_scalar, rp, xMin, yMin, xMax, yMax, light, dark);
}
