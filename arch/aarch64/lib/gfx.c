/*
 * arch/aarch64/lib/gfx.c -- gfx.library's row kernels by NEON (V7 step 2)
 *
 * Four pixels to a register; what's left of a row under four goes a
 * pixel at a time. A blend is lib/gfx/rows.h's gfx_over in 16-bit lanes,
 * a byte a channel, each pixel's alpha spread over its four bytes by a
 * table lookup; the sums fit (255 * 255 + 128 + 254 < 65536), so it
 * rounds as the scalar one does. Also built for the host's own tests
 * (lib/gfx/test.c), where the host is an Arm one.
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

static const struct gfx_rows neon = { fill_neon, copy_neon, over_neon };

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
