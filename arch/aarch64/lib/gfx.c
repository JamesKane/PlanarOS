/*
 * arch/aarch64/lib/gfx.c -- gfx.library's row kernels by NEON (V7 step 2,
 * V8 step 3)
 *
 * Four pixels to a register; what's left of a row under four goes a
 * pixel at a time. A blend is lib/gfx/rows.h's gfx_over in 16-bit lanes,
 * a byte a channel, each pixel's alpha spread over its four bytes by a
 * table lookup; the sums fit (255 * 255 + 128 + 254 < 65536), so it
 * rounds as the scalar one does. Also built for the host's own tests
 * (lib/gfx/test.c), where the host is an Arm one.
 *
 * Coverage (mask) takes 16 pixels at a time where they're all clear or
 * all covered, as most of a filled shape's are; a pixel between needs
 * three linear-light tables looked up a channel, and NEON has no gather,
 * so those are gfx_mask_px's, one at a time. The sweep is a prefix sum
 * across four lanes in two shifted adds, carried from register to
 * register, then gfx_sweep_px's folding and scaling eight at a time.
 */
#include <arm_neon.h>

#include "../../../lib/gfx/rows.h"

static void fill_neon(uint32_t *dst, uint32_t pixel, uint64_t n)
{
    uint32x4_t v = vdupq_n_u32(pixel);

    for (; n >= 8; n -= 8, dst += 8) {
        vst1q_u32(dst, v);
        vst1q_u32(dst + 4, v);
    }
    for (; n; n--)
        *dst++ = pixel;
}

static void copy_neon(uint32_t *dst, const uint32_t *src, uint64_t n)
{
    for (; n >= 8; n -= 8, dst += 8, src += 8) {
        uint32x4_t a = vld1q_u32(src), b = vld1q_u32(src + 4);

        vst1q_u32(dst, a);
        vst1q_u32(dst + 4, b);
    }
    for (; n; n--)
        *dst++ = *src++;
}

static uint16x8_t round255(uint16x8_t t)
{
    t = vaddq_u16(t, vdupq_n_u16(128));
    return vaddq_u16(t, vshrq_n_u16(t, 8));
}

static void over_neon(uint32_t *dst, const uint32_t *src, uint64_t n)
{
    static const uint8_t alpha[16] = { 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15 };
    uint8x16_t idx = vld1q_u8(alpha), opaque = vreinterpretq_u8_u32(vdupq_n_u32(0xff000000u));

    for (; n >= 4; n -= 4, dst += 4, src += 4) {
        uint8x16_t s = vreinterpretq_u8_u32(vld1q_u32(src));
        uint8x16_t d = vreinterpretq_u8_u32(vld1q_u32(dst));
        uint8x16_t a = vqtbl1q_u8(s, idx), na = vmvnq_u8(a);
        uint16x8_t lo, hi;

        s = vorrq_u8(s, opaque);
        lo = vmlal_u8(vmull_u8(vget_low_u8(s), vget_low_u8(a)), vget_low_u8(d), vget_low_u8(na));
        hi = vmlal_high_u8(vmull_high_u8(s, a), d, na);
        vst1q_u32(dst, vreinterpretq_u32_u8(vshrn_high_n_u16(vshrn_n_u16(round255(lo), 8),
                                                            round255(hi), 8)));
    }
    for (; n; n--, dst++, src++)
        *dst = gfx_over(*src, *dst);
}

static void mask_neon(uint32_t *dst, const uint8_t *cov, const uint32_t *src, uint64_t n)
{
    for (; n >= 16; n -= 16, dst += 16, cov += 16, src += 16) {
        uint8x16_t c = vld1q_u8(cov);

        if (vmaxvq_u8(c) == 0)
            continue;
        if (vminvq_u8(c) == 255) {
            vst1q_u32(dst, vld1q_u32(src));
            vst1q_u32(dst + 4, vld1q_u32(src + 4));
            vst1q_u32(dst + 8, vld1q_u32(src + 8));
            vst1q_u32(dst + 12, vld1q_u32(src + 12));
            continue;
        }
        for (unsigned i = 0; i < 16; i++)
            dst[i] = gfx_mask_px(src[i], dst[i], cov[i]);
    }
    for (; n; n--)
        *dst = gfx_mask_px(*src++, *dst, *cov++), dst++;
}

/* gfx_sweep_px of four sums. */
static uint16x4_t sweep4(uint32x4_t sum, bool evenodd)
{
    uint32x4_t full = vdupq_n_u32(GFX_COVER_FULL);
    uint32x4_t v = vreinterpretq_u32_s32(vabsq_s32(vreinterpretq_s32_u32(sum)));

    if (evenodd) {
        v = vandq_u32(v, vdupq_n_u32(2 * GFX_COVER_FULL - 1));
        v = vbslq_u32(vcgtq_u32(v, full), vsubq_u32(vdupq_n_u32(2 * GFX_COVER_FULL), v), v);
    } else {
        v = vminq_u32(v, full);
    }
    v = vaddq_u32(vmulq_n_u32(v, 255), vdupq_n_u32(GFX_COVER_FULL / 2));
    return vmovn_u32(vshrq_n_u32(v, 17));
}

/* Each lane plus every one before it, plus the carry. */
static uint32x4_t prefix(uint32x4_t a, uint32x4_t carry)
{
    uint32x4_t zero = vdupq_n_u32(0);

    a = vaddq_u32(a, vextq_u32(zero, a, 3));
    a = vaddq_u32(a, vextq_u32(zero, a, 2));
    return vaddq_u32(a, carry);
}

static void sweep_neon(uint8_t *out, const int32_t *area, uint64_t n, bool evenodd)
{
    uint32x4_t carry = vdupq_n_u32(0);
    uint32_t sum;

    for (; n >= 8; n -= 8, out += 8, area += 8) {
        uint32x4_t a = prefix(vld1q_u32((const uint32_t *)area), carry);
        uint32x4_t b = prefix(vld1q_u32((const uint32_t *)area + 4), vdupq_laneq_u32(a, 3));

        carry = vdupq_laneq_u32(b, 3);
        vst1_u8(out, vmovn_u16(vcombine_u16(sweep4(a, evenodd), sweep4(b, evenodd))));
    }
    sum = vgetq_lane_u32(carry, 0);
    for (; n; n--) {
        sum += (uint32_t)*area++;
        *out++ = gfx_sweep_px(sum, evenodd);
    }
}

static const struct gfx_rows neon = { fill_neon, copy_neon, over_neon, mask_neon, sweep_neon };

void RectFillVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax)
{
    gfx_rect_fill(&neon, rp, xMin, yMin, xMax, yMax);
}

void BltBitMapVector(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
                     int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend)
{
    gfx_blt(&neon, src, xSrc, ySrc, dst, xDest, yDest, xSize, ySize, blend);
}

void DrawBevelVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
                     uint32_t light, uint32_t dark)
{
    gfx_bevel(&neon, rp, xMin, yMin, xMax, yMax, light, dark);
}

void TextVector(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count)
{
    gfx_text(&neon, rp, x, y, string, count);
}

bool PathMaskVector(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
                    uint64_t scratchSize)
{
    return gfx_path_mask(&neon, path, flags, mask, scratch, scratchSize);
}

void BltMaskVector(struct RastPort *rp, const struct GfxMask *mask, int32_t x, int32_t y)
{
    gfx_blt_mask(&neon, rp, mask, x, y);
}

void FillPathVector(struct RastPort *rp, const struct GfxPath *path, uint32_t flags)
{
    gfx_fill_path(&neon, rp, path, flags, nullptr);
}

void FillGradientVector(struct RastPort *rp, const struct GfxGradient *g, const struct GfxPath *path,
                        uint32_t flags)
{
    gfx_fill_gradient(&neon, rp, g, path, flags);
}

void TextRunVector(struct RastPort *rp, const struct GlyphRun *run, uint32_t first, uint32_t count,
                   int32_t x, int32_t y)
{
    gfx_text_run(&neon, rp, run, first, count, x, y);
}
