/*
 * lib/gfx/rows.h -- gfx.library's row kernels, and the drivers over them
 *
 * Every operation is a driver (draw.c) that clips, then calls a kernel
 * once a row: fill n pixels, copy n, blend n over n, blend n through n
 * bytes of coverage (V8 step 3), or sweep a row of a path's areas into
 * coverage. The scalar
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
    /* V8 step 3: gfx_mask_px of each pixel; gfx_sweep_px of each sum. */
    void (*mask)(uint32_t *dst, const uint8_t *cov, const uint32_t *src, uint64_t n);
    void (*sweep)(uint8_t *out, const int32_t *area, uint64_t n, bool evenodd);
};

extern const struct gfx_rows gfx_rows_scalar;

/* lib/gfx/gamma.c (tools/gfx-gamma.py). */
extern const uint16_t gfx_linear[256];
extern const uint8_t gfx_srgb[4096];

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

/*
 * One pixel of BltMask: s through coverage c over d. The weight is c
 * out of 256 (c + c / 128: 255 is 256). Each colour channel is mixed in
 * 16-bit linear light and taken back by its top 12 bits; alpha is
 * mixed as it is. 0 leaves d, and 255 is s, exactly.
 */
static inline uint32_t gfx_mask_px(uint32_t s, uint32_t d, uint32_t c)
{
    uint32_t w = c + (c >> 7), r;

    if (c == 0)
        return d;
    if (c == 255)
        return s;
    r = ((s >> 24) * w + (d >> 24) * (256 - w) + 128) >> 8 << 24;
    for (unsigned sh = 0; sh < 24; sh += 8) {
        uint32_t l = (gfx_linear[s >> sh & 0xff] * w + gfx_linear[d >> sh & 0xff] * (256 - w) + 128) >> 8;

        r |= (uint32_t)gfx_srgb[l >> 4] << sh;
    }
    return r;
}

/*
 * One pixel of a path's coverage (path.c): 'sum', the areas added up
 * along the row so far, in 1/(256 * 512) px, wrapping at 32 bits. Its
 * size is folded every two pixels' worth (even-odd) or clamped at one
 * (non-zero), and scaled to a byte, rounded.
 */
constexpr uint32_t GFX_COVER_FULL = 256 * 2 * 256;

static inline uint8_t gfx_sweep_px(uint32_t sum, bool evenodd)
{
    uint32_t v = (int32_t)sum < 0 ? -sum : sum;

    if (evenodd) {
        v &= 2 * GFX_COVER_FULL - 1;
        v = v > GFX_COVER_FULL ? 2 * GFX_COVER_FULL - v : v;
    } else if (v > GFX_COVER_FULL) {
        v = GFX_COVER_FULL;
    }
    return (uint8_t)((v * 255 + GFX_COVER_FULL / 2) >> 17);
}

static inline uint32_t *gfx_pixel_at(const struct BitMap *bm, int64_t x, int64_t y)
{
    return (uint32_t *)((uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow) + x;
}

/* The rastport's clip rectangle within its bitmap: false if empty. */
static inline bool gfx_clip_of(const struct RastPort *rp, int64_t *x0, int64_t *y0, int64_t *x1,
                               int64_t *y1)
{
    const struct BitMap *bm = rp ? rp->rp_BitMap : nullptr;

    if (!bm || !bm->bm_BaseAddr)
        return false;
    *x0 = rp->rp_Clip.MinX > 0 ? rp->rp_Clip.MinX : 0;
    *y0 = rp->rp_Clip.MinY > 0 ? rp->rp_Clip.MinY : 0;
    *x1 = rp->rp_Clip.MaxX < (int64_t)bm->bm_Width - 1 ? rp->rp_Clip.MaxX : (int64_t)bm->bm_Width - 1;
    *y1 = rp->rp_Clip.MaxY < (int64_t)bm->bm_Height - 1 ? rp->rp_Clip.MaxY : (int64_t)bm->bm_Height - 1;
    return *x0 <= *x1 && *y0 <= *y1;
}

/* The drivers (draw.c), each the operation of the same name by 'k'; a
 * rectangle in a pen; and Text in a bitmap font (text.c). */
void gfx_rect_fill_pen(const struct gfx_rows *k, struct RastPort *rp, int64_t x0, int64_t y0,
                       int64_t x1, int64_t y1, uint32_t pen);
void gfx_text_bitmap(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count);
void gfx_rect_fill(const struct gfx_rows *k, struct RastPort *rp, int32_t x0, int32_t y0,
                   int32_t x1, int32_t y1);
void gfx_blt(const struct gfx_rows *k, const struct BitMap *src, int32_t sx, int32_t sy,
             struct RastPort *dst, int32_t dx, int32_t dy, int32_t w, int32_t h, uint32_t blend);
void gfx_bevel(const struct gfx_rows *k, struct RastPort *rp, int32_t x0, int32_t y0,
               int32_t x1, int32_t y1, uint32_t light, uint32_t dark);

/* path.c, fill.c and type.c's, likewise. */
bool gfx_path_mask(const struct gfx_rows *k, const struct GfxPath *path, uint32_t flags,
                   struct GfxMask *mask, void *scratch, uint64_t scratchSize);
void gfx_blt_mask(const struct gfx_rows *k, struct RastPort *rp, const struct GfxMask *mask,
                  int32_t x, int32_t y);
void gfx_fill_path(const struct gfx_rows *k, struct RastPort *rp, const struct GfxPath *path,
                   uint32_t flags, const struct GfxGradient *g);
void gfx_fill_gradient(const struct gfx_rows *k, struct RastPort *rp, const struct GfxGradient *g,
                       const struct GfxPath *path, uint32_t flags);
void gfx_text_run(const struct gfx_rows *k, struct RastPort *rp, const struct GlyphRun *run,
                  uint32_t first, uint32_t count, int32_t x, int32_t y);
void gfx_text(const struct gfx_rows *k, struct RastPort *rp, int32_t x, int32_t y,
              const char *string, uint64_t count);

/* arch/<isa>/lib/gfx.c: the entries by the vector unit. */
void RectFillVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax);
void BltBitMapVector(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
                     int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend);
void DrawBevelVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
                     uint32_t light, uint32_t dark);
void TextVector(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count);
bool PathMaskVector(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
                    uint64_t scratchSize);
void BltMaskVector(struct RastPort *rp, const struct GfxMask *mask, int32_t x, int32_t y);
void FillPathVector(struct RastPort *rp, const struct GfxPath *path, uint32_t flags);
void FillGradientVector(struct RastPort *rp, const struct GfxGradient *g, const struct GfxPath *path,
                        uint32_t flags);
void TextRunVector(struct RastPort *rp, const struct GlyphRun *run, uint32_t first, uint32_t count,
                   int32_t x, int32_t y);

#endif /* LIB_GFX_ROWS_H */
