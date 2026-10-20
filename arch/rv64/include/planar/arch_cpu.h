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

/*
 * This CPU's current task in one instruction, a load through tp: a
 * preemption can't come between finding the CPU and reading its task,
 * as it could if tp were copied first (review M9).
 */
static inline struct task *arch_current(void)
{
    struct task *t;
    __asm__ volatile("ld %0, %1(tp)" : "=r"(t) : "i"(offsetof(struct cpu, current)));
    return t;
}

#endif /* PLANAR_ARCH_CPU_H */
