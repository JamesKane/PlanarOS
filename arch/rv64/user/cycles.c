/*
 * arch/rv64/user/cycles.c -- the cycle counter (conformance/cycles.h)
 *
 * The kernel sets scounteren.CY, so this doesn't trap.
 */
#include "cycles.h"

uint64_t ReadCycles(void)
{
    uint64_t v;

    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

void CounterWrite(void)
{
    __asm__ volatile("csrw cycle, zero");   /* read-only: illegal */
}
