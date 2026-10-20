/*
 * arch/rv64/riscv.h -- S-mode CSRs and fences (private to arch/rv64)
 */
#ifndef ARCH_RV64_RISCV_H
#define ARCH_RV64_RISCV_H

#include <stdint.h>

#define RISCV_CSR(name)                                                     \
static inline uint64_t csr_read_##name(void)                                \
{                                                                           \
    uint64_t v;                                                             \
    __asm__ volatile("csrr %0, " #name : "=r"(v));                          \
    return v;                                                               \
}                                                                           \
static inline void csr_write_##name(uint64_t v)                             \
{                                                                           \
    __asm__ volatile("csrw " #name ", %0" : : "r"(v) : "memory");           \
}

RISCV_CSR(satp)
RISCV_CSR(scause)
RISCV_CSR(sepc)
RISCV_CSR(sie)
RISCV_CSR(sstatus)
RISCV_CSR(stval)
RISCV_CSR(stvec)

constexpr uint64_t SCAUSE_INTERRUPT = 1ULL << 63;
constexpr uint64_t IRQ_S_TIMER      = 5;
constexpr uint64_t IRQ_S_EXTERNAL   = 9;

constexpr uint64_t SSTATUS_SIE      = 1ULL << 1;
constexpr uint64_t SSTATUS_SPP      = 1ULL << 8;
constexpr uint64_t SSTATUS_VS_MASK  = 3ULL << 9;
constexpr uint64_t SSTATUS_FS       = 3ULL << 13;
constexpr uint64_t SIE_STIE         = 1ULL << IRQ_S_TIMER;
constexpr uint64_t SIE_SEIE         = 1ULL << IRQ_S_EXTERNAL;

/* Invalidate all translations, global ones included. */
static inline void sfence_vma_all(void)
{
    __asm__ volatile("sfence.vma" : : : "memory");
}

/* Accesses that may trap (trap.S). Each returns 0 and stores the value,
 * or returns the scause of the trap and stores nothing. */
uint64_t probe_ld(uint64_t addr, uint64_t *val);
uint64_t probe_stimecmp(uint64_t *val);         /* csrr stimecmp (Sstc) */

/* Point stvec at the trap entry. */
void trap_init(void);

#endif /* ARCH_RV64_RISCV_H */
