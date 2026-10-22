/*
 * lib/gfx/rows.h -- gfx.library's row kernels, and the drivers over them
 *
 * Every operation is a driver (draw.c) that clips, then calls a kernel
 * once a row: fill n pixels, copy n, or blend n over n. The scalar
 * kernels are draw.c's; arch/<isa>/lib/gfx.c has the vector unit's and
 * the interface entries that drive them, which the kernel points the
 * table at by the library's alternatives. A kernel never sees an
 * overlap: the drivers cut a blit within one row so it has none.
 */
#ifndef LIB_GFX_ROWS_H
#define LIB_GFX_ROWS_H

#include <stdint.h>

#include <planar/gfx.h>

struct gfx_rows {
    void (*fill)(uint32_t *dst, uint32_t pixel, uint64_t n);
    void (*copy)(uint32_t *dst, const uint32_t *src, uint64_t n);
    void (*over)(uint32_t *dst, const uint32_t *src, uint64_t n);
};

extern const struct gfx_rows gfx_rows_scalar;

/*
 * One channel of BLEND_SRC_OVER: s over d by alpha a, rounded to the
 * nearest (x / 255 exactly, for x to 255 * 255). Alpha is blended as a
 * channel whose source is 255, which gives a + d * (255 - a) / 255. The
 * vector kernels compute exactly this, so they agree to the bit.
 */
static inline uint32_t gfx_mix(uint32_t s, uint32_t d, uint32_t a)
{
    uint32_t t = s * a + d * (255 - a) + 128;

    return (t + (t >> 8)) >> 8;
}

static inline uint32_t gfx_over(uint32_t s, uint32_t d)
{
    uint32_t a = s >> 24, r = 0;

    s |= 0xff000000u;
    for (unsigned sh = 0; sh < 32; sh += 8)
        r |= gfx_mix(s >> sh & 0xff, d >> sh & 0xff, a) << sh;
    return r;
}

/* The drivers (draw.c), each the operation of the same name by 'k'. */
void gfx_rect_fill(const struct gfx_rows *k, struct RastPort *rp, int32_t x0, int32_t y0,
                   int32_t x1, int32_t y1);
void gfx_blt(const struct gfx_rows *k, const struct BitMap *src, int32_t sx, int32_t sy,
             struct RastPort *dst, int32_t dx, int32_t dy, int32_t w, int32_t h, uint32_t blend);
void gfx_bevel(const struct gfx_rows *k, struct RastPort *rp, int32_t x0, int32_t y0,
               int32_t x1, int32_t y1, uint32_t light, uint32_t dark);

/* arch/<isa>/lib/gfx.c: the entries by the vector unit. */
void RectFillVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax);
void BltBitMapVector(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
                     int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend);
void DrawBevelVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
                     uint32_t light, uint32_t dark);

#endif /* LIB_GFX_ROWS_H */
