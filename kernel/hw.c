/*
 * kernel/hw.c -- which firmware description the kernel reads
 */
#include "acpi.h"
#include "arch.h"
#include "bootargs.h"
#include "console.h"
#include "cpu.h"
#include "fdt.h"
#include "bootinfo.h"
#include "hw.h"
#include "intc.h"
#include "kprintf.h"
#include "memmap.h"

static enum hw_source source;

enum hw_source hw_choose(bool fdt, bool acpi, bool want_acpi)
{
    if (acpi && (want_acpi || !fdt))
        return HW_ACPI;
    return fdt ? HW_FDT : HW_NONE;
}

enum hw_source hw_init(uint64_t dtb, uint64_t rsdp)
{
    bool fdt = dtb && fdt_init(phys_to_virt(dtb));
    bool acpi = rsdp && acpi_init(phys_to_virt(rsdp));

    bootargs_init();
    source = hw_choose(fdt, acpi, bootargs_has("hw=acpi"));
    if (source != HW_FDT && fdt) {
        fdt_init(nullptr);
        bootargs_init();            /* they may have come from the tree */
    }
    if (source != HW_ACPI)
        acpi_init(nullptr);
    return source;
}

enum hw_source hw_source(void)
{
    return source;
}

const char *hw_source_name(enum hw_source s)
{
    switch (s) {
    case HW_FDT:  return "the device tree";
    case HW_ACPI: return "ACPI";
    default:      return "nothing";
    }
}

void hw_report(void)
{
    uint64_t base, size, sorted[MAX_CPUS];

    for (unsigned i = 0; memmap_ram(i, &base, &size); i++)
        kprintf("hw: ram %p-%p\n", (void *)base, (void *)(base + size - 1));

    /* The CPUs by ID, not in the order the source lists them. */
    for (unsigned i = 0; i < ncpus; i++) {
        unsigned j = i;

        for (; j > 0 && sorted[j - 1] > cpus[i].hwid; j--)
            sorted[j] = sorted[j - 1];
        sorted[j] = cpus[i].hwid;
    }
    kprintf("hw: cpus");
    for (unsigned i = 0; i < ncpus; i++)
        kprintf(" %llx", (unsigned long long)sorted[i]);
    kprintf("; boot %llx\n", (unsigned long long)cpus[0].hwid);

    if (intc()) {
        kprintf("hw: intc %s", intc()->name);
        if (intc()->describe)
            intc()->describe();
        kprintf("\n");
    }
    kprintf("hw: timer irq %u\n", arch_timer_irq());
    console_describe();
    /* Not a fact: QEMU's -kernel boot has none, its EFI boots ramfb's. */
    if (bootinfo() && bootinfo()->fb.base)
        kprintf("planar: framebuffer %ux%u at %p, %u pixels a line, GOP format %u\n",
                bootinfo()->fb.width, bootinfo()->fb.height, (void *)bootinfo()->fb.base,
                bootinfo()->fb.stride, bootinfo()->fb.format);
}
