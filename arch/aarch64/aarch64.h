/*
 * arch/aarch64/aarch64.h -- system registers and barriers (private to arch/aarch64)
 */
#ifndef ARCH_AARCH64_AARCH64_H
#define ARCH_AARCH64_AARCH64_H

#include <stdint.h>

#define sysreg_read(name)                                                   \
    __extension__ ({ uint64_t v_; __asm__ volatile("mrs %0, " #name : "=r"(v_)); v_; })
#define sysreg_write(name, v)                                               \
    __asm__ volatile("msr " #name ", %0" : : "r"((uint64_t)(v)) : "memory")

static inline void isb(void)
{
    __asm__ volatile("isb" : : : "memory");
}

/* Drop every EL1 translation on this CPU, global ones included. */
static inline void tlb_flush_local(void)
{
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1\n\tdsb nsh\n\tisb" : : : "memory");
}


/* Lazy FP/SIMD (lazy.c): before every return to EL0, and on a trapped
 * first use, for the running task's context. */
struct arch_context;
void aarch64_lazy_return(struct arch_context *ctx);
bool aarch64_lazy_first_use(struct arch_context *ctx);

/* Accesses that may fault (vectors.S): 0 and the value, or the ESR. */
uint64_t probe_ld(uint64_t addr, uint64_t *val);

#endif /* ARCH_AARCH64_AARCH64_H */
