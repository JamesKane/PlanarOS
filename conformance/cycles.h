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

/* Try to change the counters' controls, which user mode may only read:
 * must fault (review M2). */
void CounterWrite(void);

/*
 * Each CPU has its own counter, and they needn't agree (the X60's
 * don't): a task that moves between reads gets nonsense. So time one
 * operation at a time and drop what can't be right.
 */
struct CycleSum {
    uint64_t sum, count, dropped;
};

static inline void CycleAdd(struct CycleSum *s, uint64_t from, uint64_t to)
{
    uint64_t d = to - from;

    if (d > 10000000) {
        s->dropped++;
    } else {
        s->sum += d;
        s->count++;
    }
}

static inline uint64_t CycleMean(const struct CycleSum *s)
{
    return s->count ? s->sum / s->count : 0;
}

#endif /* CONFORMANCE_CYCLES_H */
