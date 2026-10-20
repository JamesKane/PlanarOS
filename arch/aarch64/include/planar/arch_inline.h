/*
 * planar/arch_inline.h -- AArch64 inline helpers
 *
 * Every arch/<isa>/include/ has a header with this name and these
 * definitions. The build puts exactly one of them on the include path.
 */
#ifndef PLANAR_ARCH_INLINE_H
#define PLANAR_ARCH_INLINE_H

#include <stdint.h>

/*
 * The kernel half (docs §3.2): TTBR1 maps every physical address P at
 * ARCH_DIRECT_MAP + P, the kernel included. entry.S maps the first 1 TiB
 * of physical addresses as device memory and the kernel image as normal
 * memory; arch_memory_init() makes the rest of RAM normal.
 */
#define ARCH_DIRECT_MAP     0xffff000000000000ULL
#define ARCH_DIRECT_MAP_GIB 1024

/* TTBR0's half with 48-bit addresses; everything above belongs to TTBR1. */
#define ARCH_USER_TOP       0x0001000000000000ULL

/*
 * A kernel task's saved state between switches (switch.S): x19-x28, the
 * frame pointer, the link register and the stack pointer. Everything
 * else is dead across arch_switch() or in a trap frame on the stack.
 */
struct arch_context {
    uint64_t x[10];     /* x19-x28 */
    uint64_t fp, lr, sp;
    uint64_t tpidr;     /* TPIDR_EL0: the user's, switched with the task (review M3) */
    uint64_t *lazy;     /* FP/SIMD save area (a page), or nullptr */
    int lazy_cpu;       /* where the registers were last loaded from it, or -1 */
};

/*
 * Per CPU (struct cpu's 'arch'): whose FP/SIMD state the registers hold,
 * and whether it may have been written since it was last saved. AArch64
 * has no dirty bit: access on means possibly written (lazy.c).
 */
struct arch_cpu {
    struct arch_context *fp_owner;
    bool fp_dirty;
    _Atomic uint64_t first_uses, loads, saves, reuses;  /* this CPU's alone writes */
};

/* A protection domain: a TTBR0 table (4 levels, 48 bits) and its ASID. */
struct arch_domain {
    uint64_t root;      /* physical */
    uint32_t asid;
};

static inline void *phys_to_virt(uint64_t pa)
{
    return (void *)(uintptr_t)(ARCH_DIRECT_MAP + pa);
}

static inline uint64_t virt_to_phys(const void *va)
{
    return (uint64_t)(uintptr_t)va - ARCH_DIRECT_MAP;
}

/* Cycle counter: PMCCNTR_EL0, which arch_early_init() starts. */
static inline uint64_t arch_read_cycles(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

/* Monotonic clock in timebase ticks: the generic timer's virtual count. */
static inline uint64_t arch_read_time(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

/* Inside a spin-wait loop. */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile("yield" : : : "memory");
}

#endif /* PLANAR_ARCH_INLINE_H */
