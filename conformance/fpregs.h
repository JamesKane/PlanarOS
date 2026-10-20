/*
 * conformance/fpregs.h -- holding the FP and vector registers (per ISA)
 *
 * arch/<isa>/user/fpregs.c fills all 32 FP registers and all 32 vector
 * registers from in[], sets the rounding mode to 'mode', spins until the
 * clock has moved 'ticks', and stores what the registers hold then.
 * Where the two register files overlap (AArch64: d<n> is the low half of
 * v<n>), the expected vector contents say so. It also sets the ISA's
 * other per-task register to 'extra' and reads it back after the spin:
 * vstart on RISC-V (which whole-register saves must not trip over,
 * review H3), TPIDR_EL0 on AArch64 (which must not leak, review M3).
 */
#ifndef CONFORMANCE_FPREGS_H
#define CONFORMANCE_FPREGS_H

#include <stdint.h>

constexpr unsigned FPREGS_MAX_VEC_BYTES = 256;

/* Bytes per vector register. */
uint64_t fpregs_vec_bytes(void);

/* misc[0]: the rounding mode read back; misc[1]: 'extra' read back. */
void fpregs_hold(const uint64_t in[32], uint64_t fout[32], uint64_t *vout,
                 uint64_t misc[2], uint64_t mode, uint64_t extra, uint64_t ticks);

/* A value for 'extra' the register can hold, made from 'seed'. */
uint64_t fpregs_extra(uint64_t seed);

/* Run a vector-FP instruction as the task's first use of the FP state:
 * returns 42 (review H6; trivially so where FP and vector are one). */
uint64_t fpregs_vecfp_first(void);

/* What 64-bit element 'e' of vector register r should hold, filled from 'in'. */
uint64_t fpregs_vec_expect(uint64_t in, unsigned e);

/* How many rounding modes there are to cycle through. */
uint64_t fpregs_modes(void);

#endif /* CONFORMANCE_FPREGS_H */
