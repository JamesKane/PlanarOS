/*
 * planar/arch_cpu.h -- this_cpu() for AArch64
 *
 * The kernel keeps its struct cpu in TPIDR_EL1, which user mode can't
 * read. Included from kernel/cpu.h only.
 */
#ifndef PLANAR_ARCH_CPU_H
#define PLANAR_ARCH_CPU_H

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

/*
 * This CPU's current task, with interrupts held off between reading
 * TPIDR_EL1 and loading through it: a task preempted in between and
 * moved would read another CPU's (review M9).
 */
static inline struct task *arch_current(void)
{
    struct task *t;
    uint64_t daif;

    __asm__ volatile("mrs %0, daif\n\t"
                     "msr daifset, #2\n\t"
                     "mrs %1, tpidr_el1\n\t"
                     "ldr %1, [%1, %2]\n\t"
                     "msr daif, %0"
                     : "=&r"(daif), "=&r"(t) : "i"(offsetof(struct cpu, current)) : "memory");
    return t;
}

#endif /* PLANAR_ARCH_CPU_H */
