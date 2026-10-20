/*
 * arch/aarch64/user/cycles.c -- the cycle counter (conformance/cycles.h)
 *
 * The kernel sets PMUSERENR_EL0.EN, so this doesn't trap.
 */
#include "cycles.h"

uint64_t ReadCycles(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}
