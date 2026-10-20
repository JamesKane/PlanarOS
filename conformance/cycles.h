/*
 * conformance/cycles.h -- the CPU's cycle counter, from user mode (per ISA)
 *
 * arch/<isa>/user/cycles.c: rdcycle on RISC-V, PMCCNTR_EL0 on AArch64.
 * For timing, not for correctness: a task can move between CPUs.
 */
#ifndef CONFORMANCE_CYCLES_H
#define CONFORMANCE_CYCLES_H

#include <stdint.h>

uint64_t ReadCycles(void);

#endif /* CONFORMANCE_CYCLES_H */
