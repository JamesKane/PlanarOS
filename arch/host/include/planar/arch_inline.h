/*
 * planar/arch_inline.h -- hosted build inline helpers
 *
 * Every arch/<isa>/include/ has a header with this name and these
 * definitions. The build puts exactly one of them on the include path.
 * The host process has no physical memory of its own: "physical"
 * addresses are its own pointers.
 */
#ifndef PLANAR_ARCH_INLINE_H
#define PLANAR_ARCH_INLINE_H

#include <stdint.h>
#include <time.h>
#include <ucontext.h>

#define ARCH_DIRECT_MAP     0ULL
#define ARCH_USER_TOP       0ULL

/* ucontext stands in for the context switch. */
struct arch_context {
    ucontext_t uc;
};

struct arch_cpu {
    int unused;
};

/* No protection in a host process. */
struct arch_domain {
    uint64_t unused;
};

static inline void *phys_to_virt(uint64_t pa)
{
    return (void *)(uintptr_t)pa;
}

static inline uint64_t virt_to_phys(const void *va)
{
    return (uint64_t)(uintptr_t)va;
}

/* Nanoseconds stand in for cycles and for timebase ticks. */
static inline uint64_t arch_read_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static inline uint64_t arch_read_cycles(void)
{
    return arch_read_time();
}

static inline void arch_cpu_relax(void)
{
}

#endif /* PLANAR_ARCH_INLINE_H */
