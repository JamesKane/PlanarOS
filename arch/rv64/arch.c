/*
 * arch/rv64/arch.c -- RV64 in S-mode under SBI firmware
 */
#include "arch.h"

const char arch_name[] = "rv64";

void arch_early_init(void)
{
}

const char *arch_privilege(void)
{
    return "S-mode";
}

[[noreturn]] void arch_halt(void)
{
    __asm__ volatile("csrw sie, zero");
    for (;;)
        __asm__ volatile("wfi");
}
