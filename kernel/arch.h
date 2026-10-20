/*
 * kernel/arch.h -- what every architecture provides (docs §2.6)
 *
 * Implemented once per directory under arch/. The build (mk/arch-*.mk)
 * chooses which arch/ directory is linked in; a missing function is a
 * link error. Small functions that must inline (phys_to_virt, the clock,
 * the cycle counter) come from <planar/arch_inline.h>, which each
 * arch/<isa>/include/ supplies under the same name.
 *
 * Grows with V1: step 2 adds CPU start and IPIs, step 3 the context
 * switch, step 4 domains (§3.5) and user mode.
 */
#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <stddef.h>
#include <stdint.h>

#include <planar/arch_inline.h>

struct intc_driver;

/* Short architecture name, for the boot banner. */
extern const char arch_name[];

/* First thing kmain() does: traps, and whatever the entry code left for
 * C (such as dropping the boot identity mapping). */
void arch_early_init(void);

/* Once the memory map is known (memmap_init): give RAM in the direct map
 * the memory type it needs. Until then RAM outside the kernel image may
 * be mapped as device memory. */
void arch_memory_init(void);

/* The privilege level the kernel runs at, for the boot banner. */
const char *arch_privilege(void);

/* The physical range the kernel image occupies, .bss included; 0, 0 if
 * the kernel isn't in managed memory (the hosted build). */
void arch_kernel_extent(uint64_t *start, uint64_t *end);

/* The interrupt controllers this architecture can have, for intc_init()
 * to match against the firmware description. nullptr-terminated. */
extern const struct intc_driver *const arch_intc_drivers[];

/* Rate of arch_read_time(), in ticks per second. */
uint64_t arch_timebase_hz(void);

/* The timer's interrupt-controller source, or 0 if the timer has a trap
 * cause of its own (RISC-V) and calls irq_timer() directly. */
uint32_t arch_timer_irq(void);

/* What drives the timer, for the boot log; nullptr if nothing raises
 * timer interrupts (the hosted build). */
const char *arch_timer_name(void);

/* Raise the timer interrupt once arch_read_time() reaches 'when'; ~0
 * disarms it. This CPU's timer only. */
void arch_timer_arm(uint64_t when);

/* Take interrupts on this CPU, or hold them off. Pending interrupts
 * wait; nothing is lost. */
void arch_irq_enable(void);
void arch_irq_disable(void);

/* Hold interrupts off and say whether they were on; put that back. */
unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long was);

/* Sleep until an enabled interrupt is pending, even with them held off. */
void arch_wait_for_interrupt(void);

/*
 * SMP (V1 step 2). arch_set_this_cpu() points this_cpu() at 'c' on the
 * calling CPU. arch_cpu_start() asks the firmware (SBI HSM, PSCI) to
 * start c->hwid at the secondary entry, which turns the MMU on, sets
 * this_cpu() and the stack from c, and calls kmain_secondary(c); that
 * calls arch_cpu_init() first. arch_send_ipi() interrupts CPU 'c', which
 * calls ipi_handle(); arch_ipi_irq() is the interrupt-controller source
 * IPIs arrive on, or 0 if they have a trap cause of their own (RISC-V).
 */
struct cpu;
void arch_set_this_cpu(struct cpu *c);
bool arch_cpu_start(struct cpu *c);
void arch_cpu_init(void);
void arch_send_ipi(struct cpu *c);
uint32_t arch_ipi_irq(void);

/*
 * Kernel tasks (V1 step 3). arch_context_init() makes 'ctx' start in
 * task_start() on the stack that ends at 'stack_top' when it's first
 * switched to. arch_switch() saves the running task's state in 'from'
 * and resumes 'to'; it returns when something switches back to 'from'.
 * Called with interrupts held off.
 */
void arch_context_init(struct arch_context *ctx, void *stack_top);
void arch_switch(struct arch_context *from, struct arch_context *to);

/*
 * Lazily switched state (FP, vector; V1 step 4b). arch_switch_out() runs
 * before every switch away from 'ctx' on this CPU: it saves what the
 * registers hold for it if that was written, so no CPU ever holds state
 * a task needs elsewhere. arch_context_free() gives back what a dead
 * task's context holds beyond its registers.
 */
void arch_switch_out(struct arch_context *ctx);
void arch_context_free(struct arch_context *ctx);

struct arch_lazy_counts {
    uint64_t first_uses, loads, saves, reuses;
};
void arch_lazy_counts(struct arch_lazy_counts *c);

/*
 * Protection domains and user mode (V1 step 4; docs §3.5 is where the
 * contract is going). A domain maps user pages in the lower half; the
 * kernel half is the same in all of them. struct arch_domain comes from
 * <planar/arch_inline.h>. Mappings take effect on every CPU: a page
 * unmapped, or a domain destroyed, is gone from every TLB on return.
 */
constexpr uint32_t MAP_READ  = 1u << 0;
constexpr uint32_t MAP_WRITE = 1u << 1;
constexpr uint32_t MAP_EXEC  = 1u << 2;
constexpr uint32_t MAP_USER  = 1u << 3;

bool arch_domain_create(struct arch_domain *d);
void arch_domain_destroy(struct arch_domain *d);   /* tables and ASID, not pages */
bool arch_map(struct arch_domain *d, uint64_t va, uint64_t pa, uint32_t flags);
uint64_t arch_unmap(struct arch_domain *d, uint64_t va);  /* the page's pa, or 0 */
uint64_t arch_lookup(struct arch_domain *d, uint64_t va); /* the page's pa, or 0 */

/* Load 'd' on this CPU (nullptr: the kernel's alone). */
void arch_domain_switch(struct arch_domain *d);

/* ASIDs in use, for leak checks. */
unsigned arch_domains_live(void);

/* Leave the kernel for user mode at 'pc' with 'sp', and 'arg0' and 'arg1'
 * as the first two arguments, in the running task's domain. The task
 * comes back through the trap path. */
[[noreturn]] void arch_user_enter(uint64_t pc, uint64_t sp, uint64_t arg0, uint64_t arg1);

/* Copy from or to a user address, which may be bad: false if it faulted. */
bool arch_copy_from_user(void *dst, uint64_t src, size_t n);
bool arch_copy_to_user(uint64_t dst, const void *src, size_t n);

/* Make every CPU's instruction fetch see code just written through the
 * kernel's mapping at [va, va + len). */
void arch_sync_icache(const void *va, size_t len);

/* Read 64 bits at kernel address 'addr' if that doesn't fault: true and
 * the value, or false. For self-tests and probes. */
bool arch_probe_read(uint64_t addr, uint64_t *val);

/* Stop this CPU for good: interrupts off, wait forever. */
[[noreturn]] void arch_halt(void);

#endif /* KERNEL_ARCH_H */
