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
#include "userimg.h"

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

[[noreturn]] void task_start(void);

/* makecontext passes ints and needs a function of its own. */
static void host_task_start(void)
{
    task_start();
}

void arch_context_init(struct arch_context *ctx, void *stack_top)
{
    /* The stack's size isn't passed; the scheduler's stacks are 16 KiB
     * and end at stack_top. */
    getcontext(&ctx->uc);
    ctx->uc.uc_stack.ss_sp = (char *)stack_top - 16384;
    ctx->uc.uc_stack.ss_size = 16384;
    ctx->uc.uc_link = nullptr;
    makecontext(&ctx->uc, host_task_start, 0);
}

void arch_switch(struct arch_context *from, struct arch_context *to)
{
    swapcontext(&from->uc, &to->uc);
}

void arch_switch_out(struct arch_context *)
{
}

void arch_context_free(struct arch_context *)
{
}

void arch_lazy_counts(struct arch_lazy_counts *c)
{
    *c = (struct arch_lazy_counts){ 0 };
}

/* One host process, one CPU: there's nothing else to start or interrupt. */
void arch_set_this_cpu(struct cpu *)
{
}

bool arch_cpu_start(struct cpu *)
{
    return false;
}

void arch_cpu_init(void)
{
}

void arch_send_ipi(struct cpu *)
{
}

uint32_t arch_ipi_irq(void)
{
    return 0;
}

/* No protection domains or user mode in a host process: processes
 * can't be created, so these are never reached with a real domain. */
const struct user_image user_images[] = {
    { nullptr, nullptr, nullptr },
};

unsigned arch_asid_bits(void)
{
    return 0;
}

bool arch_domain_create(struct arch_domain *)
{
    return false;
}

void arch_domain_destroy(struct arch_domain *)
{
}

bool arch_map(struct arch_domain *, uint64_t, uint64_t, uint32_t)
{
    return false;
}

uint64_t arch_unmap(struct arch_domain *, uint64_t)
{
    return 0;
}

bool arch_allows(struct arch_domain *, uint64_t, uint32_t)
{
    return false;
}

uint64_t arch_lookup(struct arch_domain *, uint64_t)
{
    return 0;
}

void arch_domain_switch(struct arch_domain *)
{
}

unsigned arch_domains_live(void)
{
    return 0;
}

unsigned arch_domain_tables(struct arch_domain *)
{
    return 0;
}

[[noreturn]] void arch_user_enter(uint64_t, uint64_t, uint64_t, uint64_t)
{
    abort();
}

bool arch_copy_from_user(void *, uint64_t, size_t)
{
    return false;
}

bool arch_copy_to_user(uint64_t, const void *, size_t)
{
    return false;
}

void arch_sync_icache(const void *, size_t)
{
}

/* A host process can't take a fault and carry on. */
bool arch_probe_read(uint64_t, uint64_t *)
{
    return false;
}

[[noreturn]] void arch_reboot(void)
{
    arch_halt();
}

/* A halted host "CPU" is a finished process. */
[[noreturn]] void arch_halt(void)
{
    fflush(stdout);
    exit(0);
}
