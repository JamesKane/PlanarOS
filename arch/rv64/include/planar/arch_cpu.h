/*
 * planar/arch_cpu.h -- this_cpu() for RV64
 *
 * The kernel keeps its struct cpu in tp, which compiled kernel code never
 * touches (no thread-local storage). Included from kernel/cpu.h only.
 */
#ifndef PLANAR_ARCH_CPU_H
#define PLANAR_ARCH_CPU_H

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mv %0, tp" : "=r"(c));
    return c;
}

#endif /* PLANAR_ARCH_CPU_H */
