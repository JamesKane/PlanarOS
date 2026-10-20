/*
 * kernel/main.c -- portable kernel entry
 *
 * The architecture's entry code sets up a stack and calls kmain() on the
 * boot CPU. Nothing here knows which architecture or board it runs on.
 *
 * V0 (docs §8): read the firmware description, find the console, report
 * what the firmware handed over, and stop.
 */
#include <stdint.h>

#include "arch.h"
#include "console.h"
#include "fdt.h"
#include "kprintf.h"
#include "plat.h"

[[noreturn]] void kmain(uint64_t boot_cpu, const void *firmware_data);

[[noreturn]] void kmain(uint64_t boot_cpu, const void *firmware_data)
{
    bool have_fdt;

    arch_early_init();
    have_fdt = fdt_init(firmware_data);
    plat_early_init();

    kprintf("\nvectra: hello from cpu %llu on %s (%s, %s)\n",
            (unsigned long long)boot_cpu, plat_name, arch_name, arch_privilege());
    console_report();
    if (have_fdt)
        fdt_report();
    else
        kprintf("vectra: no device tree (firmware data %p)\n", firmware_data);

    kprintf("V0 done\n");
    arch_halt();
}
