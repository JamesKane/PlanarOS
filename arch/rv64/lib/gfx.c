/*
 * arch/rv64/lib/gfx.c -- gfx.library's row kernels by RVV 1.0 (V7 step 2)
 *
 * Built with V for the library, whose other code is rv64gc's: the kernel
 * points the table at these entries only on a hart that has it. Each
 * step takes as many pixels as vsetvli grants at e32: m8 to fill and
 * copy, m2 to blend, which needs eight registers' groups at once. A
 * blend is lib/gfx/rows.h's gfx_over, a channel at a time in 32-bit
 * lanes, so it rounds as the scalar one does.
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

static const struct gfx_rows rvv = { fill_rvv, copy_rvv, over_rvv };

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
