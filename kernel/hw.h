/*
 * kernel/hw.h -- which firmware description the kernel reads
 *
 * A board describes itself by a device tree (fdt.c) or by ACPI (acpi.c);
 * UEFI may pass both. The kernel reads one: the boot argument hw=acpi
 * chooses ACPI when both are there, and otherwise the device tree wins
 * (docs/09 R6: a DTB the loader chose replaces ACPI). The rest of the
 * kernel asks hw_source() which one to read.
 */
#ifndef KERNEL_HW_H
#define KERNEL_HW_H

#include <stdint.h>

enum hw_source { HW_NONE, HW_FDT, HW_ACPI };

/* Read the device tree at physical 'dtb' and the ACPI tables from the
 * RSDP at physical 'rsdp' (either may be 0), and the boot arguments;
 * then choose the source, and forget the other. */
enum hw_source hw_init(uint64_t dtb, uint64_t rsdp);

/* The choice itself, given which sources read and whether hw=acpi was
 * asked for. */
enum hw_source hw_choose(bool fdt, bool acpi, bool want_acpi);

enum hw_source hw_source(void);
const char *hw_source_name(enum hw_source s);

/*
 * The facts the kernel took from its source, in one form whatever the
 * source: RAM, the CPUs, the interrupt controller, the timer and the
 * console, each on a line starting "hw: ". Two boots of one machine from
 * different sources must print the same lines (tools/boot-check.py
 * --facts, make compare-hw).
 */
void hw_report(void);

#endif /* KERNEL_HW_H */
