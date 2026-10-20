/*
 * kernel/arch.h -- what every architecture provides (docs §2.6)
 *
 * Implemented once per directory under arch/. The build (mk/arch-*.mk)
 * chooses which arch/ directory is linked in; a missing function is a
 * link error. Grows with each phase: V1 adds domains (§3.5), context
 * switch, traps, timers and SMP bring-up.
 */
#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <stdint.h>

/* Short architecture name, for the boot banner. */
extern const char arch_name[];

/* First thing kmain() does: anything the entry code left for C. */
void arch_early_init(void);

/* The privilege level the kernel runs at, for the boot banner
 * ("S-mode", "EL1", "host"). */
const char *arch_privilege(void);

/* Stop this CPU for good: interrupts off, wait forever. */
[[noreturn]] void arch_halt(void);

#endif /* KERNEL_ARCH_H */
