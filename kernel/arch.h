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

/* Read 64 bits at kernel address 'addr' if that doesn't fault: true and
 * the value, or false. For self-tests and probes. */
bool arch_probe_read(uint64_t addr, uint64_t *val);

/* Stop this CPU for good: interrupts off, wait forever. */
[[noreturn]] void arch_halt(void);

#endif /* KERNEL_ARCH_H */
