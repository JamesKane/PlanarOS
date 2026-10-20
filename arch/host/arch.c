/*
 * arch/host/arch.c -- the host process as an architecture
 *
 * No interrupts, no timer hardware and no managed memory: the hosted
 * build runs the kernel's portable logic, and the self-tests that need
 * hardware report themselves skipped.
 */
#include <stdio.h>
#include <stdlib.h>

#include "arch.h"

const char arch_name[] = "host";

const struct intc_driver *const arch_intc_drivers[] = { nullptr };

void arch_early_init(void)
{
}

void arch_memory_init(void)
{
}

const char *arch_privilege(void)
{
    return "user process";
}

void arch_kernel_extent(uint64_t *start, uint64_t *end)
{
    *start = *end = 0;
}

uint64_t arch_timebase_hz(void)
{
    return 1000000000;
}

uint32_t arch_timer_irq(void)
{
    return 0;
}

const char *arch_timer_name(void)
{
    return nullptr;
}

void arch_timer_arm(uint64_t)
{
}

void arch_irq_enable(void)
{
}

void arch_irq_disable(void)
{
}

unsigned long arch_irq_save(void)
{
    return 0;
}

void arch_irq_restore(unsigned long)
{
}

void arch_wait_for_interrupt(void)
{
}

/* A host process can't take a fault and carry on. */
bool arch_probe_read(uint64_t, uint64_t *)
{
    return false;
}

/* A halted host "CPU" is a finished process. */
[[noreturn]] void arch_halt(void)
{
    fflush(stdout);
    exit(0);
}
