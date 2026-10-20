/*
 * kernel/plat.h -- what every SoC provides (docs §2.6)
 *
 * Implemented once per directory under plat/ (one per SoC; boards are
 * data). The build (mk/<platform>.mk) chooses which plat/ directory is
 * linked in. V1 adds the interrupt controller, timebase, CPU start (SBI
 * HSM or PSCI), IPIs and topology.
 */
#ifndef KERNEL_PLAT_H
#define KERNEL_PLAT_H

/* Human-readable SoC or platform name, for the boot banner. */
extern const char plat_name[];

/* Bring up the early console: normally console_init(), which drives what
 * the firmware description names, after anything the firmware didn't set
 * up. Runs after the description has been read (fdt_init). The hosted
 * platform reads a description but has no hardware to drive. */
void plat_early_init(void);

/* Once interrupts, timers and the other CPUs are up: anything the board
 * must keep doing on its own, such as servicing a watchdog. */
void plat_late_init(void);

/* Reset the board by its own means (a watchdog, a PMIC); returns if it
 * has none, and the firmware's reset is tried instead (arch_reboot). */
void plat_reboot(void);

/*
 * Write one byte to the platform's own console, waiting for room. Used
 * only when the firmware description names no console the kernel can
 * drive (kernel/console.c); may drop the byte if there is none.
 */
void plat_console_putc(char c);

#endif /* KERNEL_PLAT_H */
