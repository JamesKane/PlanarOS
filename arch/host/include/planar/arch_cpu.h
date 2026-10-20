/*
 * planar/arch_cpu.h -- this_cpu() for the hosted build
 *
 * One host thread stands in for one CPU. Included from kernel/cpu.h only.
 */
#ifndef PLANAR_ARCH_CPU_H
#define PLANAR_ARCH_CPU_H

static inline struct cpu *this_cpu(void)
{
    return &cpus[0];
}

static inline struct task *arch_current(void)
{
    return cpus[0].current;
}

#endif /* PLANAR_ARCH_CPU_H */
