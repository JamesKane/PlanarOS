/*
 * kernel/main.c -- portable kernel entry
 *
 * The architecture's entry code sets up a stack and calls kmain() on the
 * boot CPU. Nothing here knows which architecture or board it runs on.
 *
 * V1 (docs/v1-plan.md): read the firmware description, build the memory
 * map and the page allocator, take traps and interrupts (step 1), start
 * the other CPUs (step 2), and run each step's self-tests.
 */
#include <stdint.h>

#include "arch.h"
#include "bootargs.h"
#include "console.h"
#include "cpu.h"
#include "fdt.h"
#include "intc.h"
#include "kprintf.h"
#include "layout.h"
#include "ktime.h"
#include "memmap.h"
#include "plat.h"
#include "pmm.h"
#include "sched.h"
#include "selftest.h"

[[noreturn]] void kmain(uint64_t boot_cpu, uint64_t firmware_data);

[[noreturn]] void kmain(uint64_t boot_cpu, uint64_t firmware_data)
{
    bool have_fdt, have_memory = false;

    arch_set_this_cpu(&cpus[0]);
    arch_early_init();
    sched_init_cpu();
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
    if (have_fdt) {
        bootargs_init();
        layout_init();
    }
    kprintf("planar: user layout %s\n", layout_name(layout_boot()));

    if (have_fdt && cpu_enumerate(boot_cpu))
        kprintf("planar: %u CPUs; booted on %llx\n", ncpus, (unsigned long long)boot_cpu);
    else
        cpu_enumerate(boot_cpu);
    if (have_fdt && intc_init(boot_cpu))
        kprintf("planar: interrupt controller %s\n", intc()->name);
    else
        kprintf("planar: no interrupt controller\n");
    if (!time_init())
        kprintf("planar: no timer\n");
    ipi_init_cpu();
    arch_irq_enable();

    selftest_pmm();
    selftest_fault();
    selftest_timer();
    kprintf("V1.1 done\n");

    if (ncpus > 1 && pmm_total())
        kprintf("planar: %u of %u CPUs online\n", smp_start(), ncpus);
    plat_late_init();
    selftest_smp();
    selftest_ipi();
    selftest_cpu_timers();
    selftest_lock();
    kprintf("V1.2 done\n");

    /* Step 3's tests need to block, so they run as a task; this code
     * becomes the boot CPU's idle task. */
    if (!task_create("selftest", 10, 1, selftest_tasks, nullptr)) {
        kprintf("V1.3 tasks skipped: no memory for tasks\n");
        kprintf("V1.3 done\n");
        kprintf("V1.4 user skipped: no tasks\n");
        kprintf("V1.4 done\n");
        kprintf("V1.5 areas skipped: no tasks\n");
        kprintf("V1.5 done\n");
        kprintf("V2.1 ipc skipped: no tasks\n");
        kprintf("V2.1 done\n");
        kprintf("V2.2 rings skipped: no tasks\n");
        kprintf("V2.2 done\n");
        kprintf("V2.3 domsg skipped: no tasks\n");
        kprintf("V2.3 done\n");
        arch_halt();
    }
    sched_idle();
    arch_halt();
}
