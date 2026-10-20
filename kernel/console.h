/*
 * kernel/console.h -- the kernel's early console (docs §9.4, layer 0)
 *
 * Found from the firmware description: /chosen's stdout-path on FDT
 * boards, the SPCR (or a quirk's DBG2) on ACPI boards. Falls back to
 * plat_console_putc() when the description names nothing the kernel can
 * drive.
 */
#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

/* Pick the console from the firmware description. False if it names
 * none the kernel can drive; the platform's console is used instead. */
bool console_init(void);

/* Describe the console in use, for the boot log. */
void console_report(void);

/* Its "hw: console" line, for hw_report() (kernel/hw.h). */
void console_describe(void);

/* Write one byte, waiting for room. */
void console_putc(char c);

#endif /* KERNEL_CONSOLE_H */
