/*
 * arch/rv64/lib/gfx.c -- gfx.library's row kernels by RVV 1.0 (V7 step 2,
 * V8 step 3)
 *
 * Built with V for the library, whose other code is rv64gc's: the kernel
 * points the table at these entries only on a hart that has it. Each
 * step takes as many pixels as vsetvli grants at e32: m8 to fill and
 * copy, m2 to blend, which needs eight registers' groups at once. A
 * blend is lib/gfx/rows.h's gfx_over, a channel at a time in 32-bit
 * lanes, so it rounds as the scalar one does.
 *
 * Coverage (mask) is gfx_mask_px in 32-bit lanes, the linear-light
 * tables looked up by indexed loads, then the clear pixels and the
 * covered ones put back as they must be. The sweep is a prefix sum over
 * the group in log2(vl) slides and adds, carried from group to group,
 * then gfx_sweep_px's folding and scaling, narrowed to bytes.
 */
#include <riscv_vector.h>

#include "../../../lib/gfx/rows.h"

static void fill_rvv(uint32_t *dst, uint32_t pixel, uint64_t n)
{
    for (size_t vl; n; n -= vl, dst += vl) {
        vl = __riscv_vsetvl_e32m8(n);
        __riscv_vse32_v_u32m8(dst, __riscv_vmv_v_x_u32m8(pixel, vl), vl);
    }
}

static void copy_rvv(uint32_t *dst, const uint32_t *src, uint64_t n)
{
    for (size_t vl; n; n -= vl, dst += vl, src += vl) {
        vl = __riscv_vsetvl_e32m8(n);
        __riscv_vse32_v_u32m8(dst, __riscv_vle32_v_u32m8(src, vl), vl);
    }
}

static void over_rvv(uint32_t *dst, const uint32_t *src, uint64_t n)
{
    for (size_t vl; n; n -= vl, dst += vl, src += vl) {
        vl = __riscv_vsetvl_e32m2(n);
        vuint32m2_t s = __riscv_vle32_v_u32m2(src, vl);
        vuint32m2_t d = __riscv_vle32_v_u32m2(dst, vl);
        vuint32m2_t a = __riscv_vsrl_vx_u32m2(s, 24, vl);
        vuint32m2_t na = __riscv_vrsub_vx_u32m2(a, 255, vl);
        vuint32m2_t r = __riscv_vmv_v_x_u32m2(0, vl);

        s = __riscv_vor_vx_u32m2(s, 0xff000000u, vl);
        for (unsigned sh = 0; sh < 32; sh += 8) {
            vuint32m2_t sc = __riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(s, sh, vl), 0xff, vl);
            vuint32m2_t dc = __riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(d, sh, vl), 0xff, vl);
            vuint32m2_t t = __riscv_vmul_vv_u32m2(sc, a, vl);

            t = __riscv_vmacc_vv_u32m2(t, dc, na, vl);
            t = __riscv_vadd_vx_u32m2(t, 128, vl);
            t = __riscv_vsrl_vx_u32m2(__riscv_vadd_vv_u32m2(t, __riscv_vsrl_vx_u32m2(t, 8, vl), vl),
                                      8, vl);
            r = __riscv_vor_vv_u32m2(r, __riscv_vsll_vx_u32m2(t, sh, vl), vl);
        }
        __riscv_vse32_v_u32m2(dst, r, vl);
    }
}

static void mask_rvv(uint32_t *dst, const uint8_t *cov, const uint32_t *src, uint64_t n)
{
    for (size_t vl; n; n -= vl, dst += vl, cov += vl, src += vl) {
        vl = __riscv_vsetvl_e32m2(n);
        vuint32m2_t c = __riscv_vzext_vf4_u32m2(__riscv_vle8_v_u8mf2(cov, vl), vl);
        vuint32m2_t s = __riscv_vle32_v_u32m2(src, vl);
        vuint32m2_t d = __riscv_vle32_v_u32m2(dst, vl);
        vuint32m2_t w = __riscv_vadd_vv_u32m2(c, __riscv_vsrl_vx_u32m2(c, 7, vl), vl);
        vuint32m2_t nw = __riscv_vrsub_vx_u32m2(w, 256, vl);
        vuint32m2_t r = __riscv_vmul_vv_u32m2(__riscv_vsrl_vx_u32m2(s, 24, vl), w, vl);

        r = __riscv_vmacc_vv_u32m2(r, __riscv_vsrl_vx_u32m2(d, 24, vl), nw, vl);
        r = __riscv_vsll_vx_u32m2(__riscv_vsrl_vx_u32m2(__riscv_vadd_vx_u32m2(r, 128, vl), 8, vl), 24, vl);
        for (unsigned sh = 0; sh < 24; sh += 8) {
            vuint32m2_t si = __riscv_vsll_vx_u32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(s, sh, vl), 0xff, vl), 1, vl);
            vuint32m2_t di = __riscv_vsll_vx_u32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(d, sh, vl), 0xff, vl), 1, vl);
            vuint32m2_t ls = __riscv_vzext_vf2_u32m2(__riscv_vluxei32_v_u16m1(gfx_linear, si, vl), vl);
            vuint32m2_t ld = __riscv_vzext_vf2_u32m2(__riscv_vluxei32_v_u16m1(gfx_linear, di, vl), vl);
            vuint32m2_t l = __riscv_vmacc_vv_u32m2(__riscv_vmul_vv_u32m2(ls, w, vl), ld, nw, vl);

            l = __riscv_vsrl_vx_u32m2(__riscv_vadd_vx_u32m2(l, 128, vl), 12, vl);  /* >> 8, then >> 4 */
            l = __riscv_vzext_vf4_u32m2(__riscv_vluxei32_v_u8mf2(gfx_srgb, l, vl), vl);
            r = __riscv_vor_vv_u32m2(r, __riscv_vsll_vx_u32m2(l, sh, vl), vl);
        }
        r = __riscv_vmerge_vvm_u32m2(r, d, __riscv_vmseq_vx_u32m2_b16(c, 0, vl), vl);
        r = __riscv_vmerge_vvm_u32m2(r, s, __riscv_vmseq_vx_u32m2_b16(c, 255, vl), vl);
        __riscv_vse32_v_u32m2(dst, r, vl);
    }
}

static void sweep_rvv(uint8_t *out, const int32_t *area, uint64_t n, bool evenodd)
{
    uint32_t carry = 0;

    for (size_t vl; n; n -= vl, out += vl, area += vl) {
        vl = __riscv_vsetvl_e32m2(n);
        vuint32m2_t x = __riscv_vle32_v_u32m2((const uint32_t *)area, vl);
        vuint32m2_t zero = __riscv_vmv_v_x_u32m2(0, vl);
        vuint32m2_t v;

        for (size_t k = 1; k < vl; k <<= 1)
            x = __riscv_vadd_vv_u32m2(x, __riscv_vslideup_vx_u32m2(zero, x, k, vl), vl);
        x = __riscv_vadd_vx_u32m2(x, carry, vl);
        carry = __riscv_vmv_x_s_u32m2_u32(__riscv_vslidedown_vx_u32m2(x, vl - 1, vl));
        v = __riscv_vmerge_vvm_u32m2(x, __riscv_vrsub_vx_u32m2(x, 0, vl),
                                     __riscv_vmslt_vx_i32m2_b16(__riscv_vreinterpret_v_u32m2_i32m2(x), 0, vl), vl);
        if (evenodd) {
            v = __riscv_vand_vx_u32m2(v, 2 * GFX_COVER_FULL - 1, vl);
            v = __riscv_vmerge_vvm_u32m2(v, __riscv_vrsub_vx_u32m2(v, 2 * GFX_COVER_FULL, vl),
                                         __riscv_vmsgtu_vx_u32m2_b16(v, GFX_COVER_FULL, vl), vl);
        } else {
            v = __riscv_vminu_vx_u32m2(v, GFX_COVER_FULL, vl);
        }
        v = __riscv_vsrl_vx_u32m2(__riscv_vmacc_vx_u32m2(__riscv_vmv_v_x_u32m2(GFX_COVER_FULL / 2, vl), 255, v, vl), 17, vl);
        __riscv_vse8_v_u8mf2(out, __riscv_vnsrl_wx_u8mf2(__riscv_vnsrl_wx_u16m1(v, 0, vl), 0, vl), vl);
    }
}

static const struct gfx_rows rvv = { fill_rvv, copy_rvv, over_rvv, mask_rvv, sweep_rvv };

void RectFillVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax)
{
    gfx_rect_fill(&rvv, rp, xMin, yMin, xMax, yMax);
}

void BltBitMapVector(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
                     int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend)
{
    gfx_blt(&rvv, src, xSrc, ySrc, dst, xDest, yDest, xSize, ySize, blend);
}

void DrawBevelVector(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
                     uint32_t light, uint32_t dark)
{
    gfx_bevel(&rvv, rp, xMin, yMin, xMax, yMax, light, dark);
}

void TextVector(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count)
{
    gfx_text(&rvv, rp, x, y, string, count);
}

bool PathMaskVector(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
                    uint64_t scratchSize)
{
    return gfx_path_mask(&rvv, path, flags, mask, scratch, scratchSize);
}

void BltMaskVector(struct RastPort *rp, const struct GfxMask *mask, int32_t x, int32_t y)
{
    gfx_blt_mask(&rvv, rp, mask, x, y);
}

void FillPathVector(struct RastPort *rp, const struct GfxPath *path, uint32_t flags)
{
    gfx_fill_path(&rvv, rp, path, flags, nullptr);
}

void FillGradientVector(struct RastPort *rp, const struct GfxGradient *g, const struct GfxPath *path,
                        uint32_t flags)
{
    gfx_fill_gradient(&rvv, rp, g, path, flags);
}

void TextRunVector(struct RastPort *rp, const struct GlyphRun *run, uint32_t first, uint32_t count,
                   int32_t x, int32_t y)
{
    gfx_text_run(&rvv, rp, run, first, count, x, y);
}
