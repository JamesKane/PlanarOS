/*
 * arch/aarch64/user/fpregs.c -- holding v0-v31 (conformance/fpregs.h)
 *
 * The FP and SIMD registers are one file: d<n> is the low half of v<n>.
 * So each v<n> gets in[n] in its low half (what fout reads back as d<n>)
 * and ~in[n] in its high half. One asm block, so the compiler can't use
 * the registers in between.
 */
#include "fpregs.h"

uint64_t fpregs_vec_bytes(void)
{
    return 16;
}

uint64_t fpregs_vec_expect(uint64_t in, unsigned e)
{
    return e == 0 ? in : ~in;
}

uint64_t fpregs_modes(void)
{
    return 4;                   /* FPCR.RMode: RN, RP, RM, RZ */
}

void fpregs_hold(const uint64_t in[32], uint64_t fout[32], uint64_t *vout,
                 uint64_t misc[1], uint64_t mode, uint64_t ticks)
{
    __asm__ volatile(
        ".irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "ldr x9, [%[in], #\\r*8]\n"
        "fmov d\\r, x9\n"
        "mvn x9, x9\n"
        "mov v\\r\\().d[1], x9\n"
        ".endr\n"
        "mrs x9, fpcr\n"
        "bfi x9, %[mode], #22, #2\n"
        "msr fpcr, x9\n"
        "mrs x10, cntvct_el0\n"
        "add x10, x10, %[ticks]\n"
        "1: mrs x11, cntvct_el0\n"
        "cmp x11, x10\n"
        "b.lo 1b\n"
        ".irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "str d\\r, [%[fout], #\\r*8]\n"
        "str q\\r, [%[vout], #\\r*16]\n"
        ".endr\n"
        "mrs x9, fpcr\n"
        "ubfx x9, x9, #22, #2\n"
        "str x9, [%[misc]]\n"
        :
        : [in] "r"(in), [fout] "r"(fout), [misc] "r"(misc), [vout] "r"(vout),
          [mode] "r"(mode), [ticks] "r"(ticks)
        : "x9", "x10", "x11", "memory", "cc",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11",
          "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22",
          "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}
