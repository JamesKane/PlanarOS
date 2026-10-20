/*
 * kernel/main.c -- portable kernel entry
 *
 * The architecture's entry code sets up a stack and calls kmain() on the
 * boot CPU. Nothing here knows which architecture or board it runs on.
 *
 * V1 step 1 (docs/v1-plan.md): read the firmware description, build the
 * memory map and the page allocator, take traps and interrupts, and run
 * the self-tests.
 */
#include <stdint.h>

#include "arch.h"
#include "console.h"
#include "fdt.h"
#include "intc.h"
#include "kprintf.h"
#include "ktime.h"
#include "memmap.h"
#include "plat.h"
#include "pmm.h"
#include "selftest.h"

[[noreturn]] void kmain(uint64_t boot_cpu, uint64_t firmware_data);

[[noreturn]] void kmain(uint64_t boot_cpu, uint64_t firmware_data)
{
    bool have_fdt, have_memory = false;

    arch_early_init();
    have_fdt = firmware_data && fdt_init(phys_to_virt(firmware_data));
    if (have_fdt && (have_memory = memmap_init()))
        arch_memory_init();
    plat_early_init();

    kprintf("\nplanar: hello from cpu %llx on %s (%s, %s)\n",
            (unsigned long long)boot_cpu, plat_name, arch_name, arch_privilege());
    console_report();
    if (have_fdt)
        fdt_report();
    else
        kprintf("planar: no device tree (firmware data %p)\n", (void *)firmware_data);
    if (have_memory) {
        memmap_report();
        pmm_init();
    }

    if (have_fdt && intc_init(boot_cpu))
        kprintf("planar: interrupt controller %s\n", intc()->name);
    else
        kprintf("planar: no interrupt controller\n");
    if (!time_init())
        kprintf("planar: no timer\n");
    arch_irq_enable();

    selftest_pmm();
    selftest_fault();
    selftest_timer();

    kprintf("V1.1 done\n");
    arch_halt();
}
