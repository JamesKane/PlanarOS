/*
 * kernel/cpuclass.h -- core classes: which CPUs are alike, and how fast
 */
#ifndef KERNEL_CPUCLASS_H
#define KERNEL_CPUCLASS_H

#include <stdint.h>

constexpr unsigned CPU_NO_CLASS = ~0u;

struct cpu_class {
    uint64_t type;                  /* arch_cpu_type() */
    const char *name;               /* arch_cpu_type_name(); nullptr if unknown */
    unsigned ncpus, nclusters;      /* nclusters 0: the firmware doesn't say */
    uint64_t median_ticks;          /* the workload, its CPUs' median best time */
    uint64_t min_ticks, max_ticks;  /* and the fastest and slowest of them */
    uint64_t capacity;              /* median against the fastest class's, which is 1024 */
};

/* Time the workload on every online CPU (each in turn, the rest idle),
 * group the CPUs by core type, and give each its class's capacity. On
 * the boot CPU, once the others are online. */
void cpu_classify(void);

/* Print the classes. */
void cpu_class_report(void);

/* Class 'i', in the order of each's first CPU; nullptr past the end. */
const struct cpu_class *cpu_class(unsigned i);
unsigned cpu_nclasses(void);

/* How many PPTT clusters hold CPUs of more than one type (the PPTT
 * says they're alike). */
unsigned cpu_clusters_mixed(void);

#endif /* KERNEL_CPUCLASS_H */
