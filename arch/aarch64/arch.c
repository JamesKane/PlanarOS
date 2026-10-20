/*
 * arch/aarch64/arch.c -- AArch64 at EL1
 */
#include <stdint.h>

#include "arch.h"

const char arch_name[] = "aarch64";

void arch_early_init(void)
{
}

/* The entry code drops to EL1, so this reports EL1 unless that failed. */
const char *arch_privilege(void)
{
    uint64_t el;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    switch ((el >> 2) & 3) {
    case 1:  return "EL1";
    case 2:  return "EL2";
    default: return "EL?";
    }
}

[[noreturn]] void arch_halt(void)
{
    __asm__ volatile("msr daifset, #0xf");
    for (;;)
        __asm__ volatile("wfi");
}
