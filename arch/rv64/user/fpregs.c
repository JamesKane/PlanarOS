/*
 * arch/rv64/user/fpregs.c -- holding f0-f31 and v0-v31 (conformance/fpregs.h)
 *
 * One asm block, so the compiler can't use the registers in between.
 * From omega's conformance/fpu.c.
 */
#include "fpregs.h"

uint64_t fpregs_vec_bytes(void)
{
    uint64_t v;

    __asm__ volatile("csrr %0, vlenb" : "=r"(v));
    return v;
}

uint64_t fpregs_vec_expect(uint64_t in, unsigned)
{
    return ~in;
}

uint64_t fpregs_modes(void)
{
    return 5;                   /* RNE, RTZ, RDN, RUP, RMM */
}

uint64_t fpregs_extra(uint64_t seed)
{
    return 1 + seed % 4;        /* a vstart any vector length allows */
}

uint64_t fpregs_vecfp_first(void)
{
    /* vsetivli loads the vector state; vfmv.v.f then needs the FP state
     * as well, and nothing before it touched FP (f0's value is whatever
     * it is: only reaching the end matters). */
    __asm__ volatile("vsetivli zero, 1, e64, m1, ta, ma\n"
                     "vfmv.v.f v1, f0\n" : : : "v1");
    return 42;
}

void fpregs_hold(const uint64_t in[32], uint64_t fout[32], uint64_t *vout,
                 uint64_t misc[2], uint64_t mode, uint64_t extra, uint64_t ticks)
{
    __asm__ volatile(
        ".irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "fld f\\r, \\r*8(%[in])\n"
        ".endr\n"
        "fsrm %[mode]\n"
        "vsetvli t1, zero, e64, m1, ta, ma\n"
        ".irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "ld t0, \\r*8(%[in])\n"
        "not t0, t0\n"
        "vmv.v.x v\\r, t0\n"
        ".endr\n"
        "csrw vstart, %[extra]\n"
        "rdtime t3\n"
        "add t3, t3, %[ticks]\n"
        "1: rdtime t4\n"
        "bltu t4, t3, 1b\n"
        ".irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "fsd f\\r, \\r*8(%[fout])\n"
        ".endr\n"
        "frrm t1\n"
        "sd t1, 0(%[misc])\n"
        "csrr t1, vstart\n"
        "sd t1, 8(%[misc])\n"
        "csrw vstart, zero\n"
        "csrr t1, vlenb\n"
        "mv t2, %[vout]\n"
        ".irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "vs1r.v v\\r, (t2)\n"
        "add t2, t2, t1\n"
        ".endr\n"
        :
        : [in] "r"(in), [fout] "r"(fout), [misc] "r"(misc), [vout] "r"(vout),
          [mode] "r"(mode), [extra] "r"(extra), [ticks] "r"(ticks)
        : "t0", "t1", "t2", "t3", "t4", "memory",
          "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11",
          "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22",
          "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11",
          "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22",
          "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}
