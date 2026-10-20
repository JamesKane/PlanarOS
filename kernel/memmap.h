/*
 * kernel/memmap.h -- the physical memory map, from the firmware description
 *
 * Part of what the kernel reads before any server runs (docs §9.4,
 * layer 0): where RAM is, and which of it the firmware or the boot keeps
 * for itself. Device tree for now; the EFI memory map on ACPI boards once
 * the EFI stub exists.
 *
 * RAM ranges are rounded inward to 2 MiB so the direct map can cover them
 * with blocks of one memory type (arch_memory_init); at most 4 MiB per
 * range goes unused.
 */
#ifndef KERNEL_MEMMAP_H
#define KERNEL_MEMMAP_H

#include <stdint.h>

/* Read the map. False if there's no RAM in it (the hosted build without
 * a device tree). */
bool memmap_init(void);

/* Forget RAM at and above 'top': the architecture couldn't map it. RAM
 * past the direct map is never in the map at all. */
void memmap_clip(uint64_t top);

/* Entry 'index' of RAM, or of the reserved ranges; false past the end. */
bool memmap_ram(unsigned index, uint64_t *base, uint64_t *size);
bool memmap_reserved(unsigned index, uint64_t *base, uint64_t *size);

/* Print the map. */
void memmap_report(void);

#endif /* KERNEL_MEMMAP_H */
