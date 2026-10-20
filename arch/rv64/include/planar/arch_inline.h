/*
 * planar/arch_inline.h -- RV64 inline helpers
 *
 * Every arch/<isa>/include/ has a header with this name and these
 * definitions. The build puts exactly one of them on the include path.
 */
#ifndef PLANAR_ARCH_INLINE_H
#define PLANAR_ARCH_INLINE_H

#include <stdint.h>

/*
 * The kernel half (docs §3.2): every physical address P is mapped at
 * ARCH_DIRECT_MAP + P, the kernel included, so physical memory and device
 * registers are reached through the direct map. Sv39's upper half starts
 * here; entry.S maps ARCH_DIRECT_MAP_GIB GiB of it with gigapages, which
 * on RISC-V carry no memory type: the platform's PMAs say which physical
 * ranges are devices.
 */
#define ARCH_DIRECT_MAP     0xffffffc000000000ULL
#define ARCH_DIRECT_MAP_GIB 16

/* Sv39's user half; everything above belongs to the kernel. */
#define ARCH_USER_TOP       0x0000004000000000ULL

/*
 * A kernel task's saved state between switches (switch.S): the
 * callee-saved registers and the stack pointer. Everything else is dead
 * across arch_switch() or in a trap frame on the task's stack.
 */
struct arch_context {
    uint64_t ra;
    uint64_t sp;
    uint64_t s[12];
};

/* A protection domain: an Sv39 root table and its ASID. */
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

/* Cycle counter; readable in S-mode once the firmware sets mcounteren.CY. */
static inline uint64_t arch_read_cycles(void)
{
    uint64_t v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

/* Monotonic clock in timebase ticks: a CSR read, no trap, on the X60. */
static inline uint64_t arch_read_time(void)
{
    uint64_t v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

/* Inside a spin-wait loop. */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile("nop" : : : "memory");
}

#endif /* PLANAR_ARCH_INLINE_H */
