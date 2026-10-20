/*
 * arch/aarch64/lazy.S -- saving and loading the FP/SIMD registers
 *
 * The kernel is built without FP/SIMD; only these routines use it, with
 * CPACR_EL1.FPEN switched on by the caller (lazy.c). The area: q0-q31,
 * then FPSR and FPCR.
 */

    .arch   armv8.2-a+fp+simd
    .section .text

/* void aarch64_fp_save(uint64_t *area) */
    .globl  aarch64_fp_save
aarch64_fp_save:
    stp     q0, q1, [x0, #32 * 0]
    stp     q2, q3, [x0, #32 * 1]
    stp     q4, q5, [x0, #32 * 2]
    stp     q6, q7, [x0, #32 * 3]
    stp     q8, q9, [x0, #32 * 4]
    stp     q10, q11, [x0, #32 * 5]
    stp     q12, q13, [x0, #32 * 6]
    stp     q14, q15, [x0, #32 * 7]
    stp     q16, q17, [x0, #32 * 8]
    stp     q18, q19, [x0, #32 * 9]
    stp     q20, q21, [x0, #32 * 10]
    stp     q22, q23, [x0, #32 * 11]
    stp     q24, q25, [x0, #32 * 12]
    stp     q26, q27, [x0, #32 * 13]
    stp     q28, q29, [x0, #32 * 14]
    stp     q30, q31, [x0, #32 * 15]
    mrs     x9, fpsr
    mrs     x10, fpcr
    add     x11, x0, #32 * 16       /* past stp's reach for x registers */
    stp     x9, x10, [x11]
    ret

/* void aarch64_fp_load(const uint64_t *area) */
    .globl  aarch64_fp_load
aarch64_fp_load:
    ldp     q0, q1, [x0, #32 * 0]
    ldp     q2, q3, [x0, #32 * 1]
    ldp     q4, q5, [x0, #32 * 2]
    ldp     q6, q7, [x0, #32 * 3]
    ldp     q8, q9, [x0, #32 * 4]
    ldp     q10, q11, [x0, #32 * 5]
    ldp     q12, q13, [x0, #32 * 6]
    ldp     q14, q15, [x0, #32 * 7]
    ldp     q16, q17, [x0, #32 * 8]
    ldp     q18, q19, [x0, #32 * 9]
    ldp     q20, q21, [x0, #32 * 10]
    ldp     q22, q23, [x0, #32 * 11]
    ldp     q24, q25, [x0, #32 * 12]
    ldp     q26, q27, [x0, #32 * 13]
    ldp     q28, q29, [x0, #32 * 14]
    ldp     q30, q31, [x0, #32 * 15]
    add     x11, x0, #32 * 16
    ldp     x9, x10, [x11]
    msr     fpsr, x9
    msr     fpcr, x10
    ret
