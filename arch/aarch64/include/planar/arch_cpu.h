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

#endif /* PLANAR_ARCH_CPU_H */
