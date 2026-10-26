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
#include "bootinfo.h"
#include "console.h"
#include "cpu.h"
#include "cpuclass.h"
#include "acpi.h"
#include "fdt.h"
#include "hw.h"
#include "intc.h"
#include "kprintf.h"
#include "ktime.h"
#include "library.h"
#include "memmap.h"
#include "plat.h"
#include "pmm.h"
#include "sched.h"
#include "selftest.h"

[[noreturn]] void kmain(uint64_t boot_cpu, uint64_t firmware_data);

[[noreturn]] void kmain(uint64_t boot_cpu, uint64_t firmware_data)
{
    bool have_hw, have_memory = false;
    enum hw_source hw;
    uint64_t dtb;

    arch_set_this_cpu(&cpus[0]);
    arch_early_init();
    sched_init_cpu();
    dtb = bootinfo_init(firmware_data);
    hw = hw_init(dtb, bootinfo() ? bootinfo()->rsdp : 0);
    have_hw = hw != HW_NONE;
    if (have_hw && (have_memory = memmap_init()))
        arch_memory_init();
    plat_early_init();

    kprintf("\nplanar: hello from cpu %llx on %s (%s, %s)\n",
            (unsigned long long)boot_cpu, plat_name, arch_name, arch_privilege());
    console_report();
    if (bootinfo())
        kprintf("planar: booted through UEFI\n");
    if (hw == HW_FDT) {
        fdt_report();
        if (bootargs_has("dumpdtb"))
            fdt_dump_blob();
    }
    else if (hw == HW_ACPI) {
        acpi_report();
        if (bootargs_has("dumpacpi"))
            acpi_dump_tables();
    }
    else
        kprintf("planar: no device tree or ACPI (firmware data %p)\n", (void *)firmware_data);
    if (have_memory) {
        memmap_report();
        pmm_init();
    }
    kprintf("planar: %u-bit ASIDs\n", arch_asid_bits());

    if (have_hw && cpu_enumerate(boot_cpu))
        kprintf("planar: %u CPUs; booted on %llx\n", ncpus, (unsigned long long)boot_cpu);
    else
        cpu_enumerate(boot_cpu);
    if (have_hw && intc_init(boot_cpu))
        kprintf("planar: interrupt controller %s\n", intc()->name);
    else
        kprintf("planar: no interrupt controller\n");
    if (!time_init())
        kprintf("planar: no timer\n");
    if (have_hw)
        hw_report();
    ipi_init_cpu();
    arch_irq_enable();

    selftest_pmm();
    selftest_fault();
    selftest_timer();
    selftest_fdt();
    kprintf("V1.1 done\n");

    if (ncpus > 1 && pmm_total()) {
        kprintf("planar: %u of %u CPUs online\n", smp_start(), ncpus);
    } else if (ncpus > 1) {
        /* No stacks for them (the hosted build): the rest stay off. */
        kprintf("planar: no memory to start the other %u CPUs\n", ncpus - 1);
        ncpus = 1;
    }
    cpu_classify();
    cpu_class_report();
    plat_late_init();
    selftest_smp();
    selftest_ipi();
    selftest_cpu_timers();
    selftest_lock();
    kprintf("V1.2 done\n");

    /* Before the first process: each maps the libraries from its start. */
    library_init();

    /* From here a kernel task shows the log (kernel/kprintf.c). */
    klog_start();

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
        kprintf("V2.4 death skipped: no tasks\n");
        kprintf("V2.4 done\n");
        kprintf("V3.1 boot skipped: no tasks\n");
        kprintf("V3.1 done\n");
        selftest_acpi();            /* needs no tasks */
        kprintf("V3.2 done\n");
        selftest_board();
        kprintf("V3.3 done\n");
        selftest_classes();
        kprintf("V3.4 placement skipped: no tasks\n");
        kprintf("V3.4 done\n");
        kprintf("V3.5 costs skipped: no tasks\n");
        kprintf("V3.5 done\n");
        kprintf("V3.6 tlb skipped: no tasks\n");
        kprintf("V3.6 done\n");
        kprintf("V4.1 drivers skipped: no tasks\n");
        kprintf("V4.1 done\n");
        kprintf("V4.3 hwdb skipped: no tasks\n");
        kprintf("V4.3 done\n");
        kprintf("V4.5 storage skipped: no tasks\n");
        kprintf("V4.5 done\n");
        kprintf("V4.6 files skipped: no tasks\n");
        kprintf("V4.6 done\n");
        kprintf("V4.7 shell skipped: no tasks\n");
        kprintf("V4.7 done\n");
        kprintf("V5.1 queue calls skipped: no tasks\n");
        kprintf("V5.1 done\n");
        kprintf("V5.2 power skipped: no tasks\n");
        kprintf("V5.2 done\n");
        kprintf("V5.3 display skipped: no tasks\n");
        kprintf("V5.3 done\n");
        kprintf("V5.4 input skipped: no tasks\n");
        kprintf("V5.4 done\n");
        kprintf("V5.5 gpu skipped: no tasks\n");
        kprintf("V5.5 done\n");
        kprintf("V5.6 gpu skipped: no tasks\n");
        kprintf("V5.6 done\n");
        kprintf("V5.7 load skipped: no tasks\n");
        kprintf("V5.7 done\n");
        kprintf("V6.1 runtime skipped: no tasks\n");
        kprintf("V6.1 done\n");
        kprintf("V6.2 exec memory skipped: no tasks\n");
        kprintf("V6.2 done\n");
        kprintf("V6.3 utility skipped: no tasks\n");
        kprintf("V6.3 done\n");
        kprintf("V6.4 libraries skipped: no tasks\n");
        kprintf("V6.4 done\n");
        kprintf("V6.5 clib skipped: no tasks\n");
        kprintf("V6.5 done\n");
        kprintf("V6.6 dos skipped: no tasks\n");
        kprintf("V6.6 done\n");
        kprintf("V6.7 supervision skipped: no tasks\n");
        kprintf("V6.7 done\n");
        kprintf("V6.8 debts skipped: no tasks\n");
        kprintf("V6.8 done\n");
        kprintf("V7.1 screen skipped: no tasks\n");
        kprintf("V7.1 done\n");
        kprintf("V7.2 gfx skipped: no tasks\n");
        kprintf("V7.2 done\n");
        kprintf("V7.3 windows skipped: no tasks\n");
        kprintf("V7.3 done\n");
        kprintf("V7.4 input skipped: no tasks\n");
        kprintf("V7.4 done\n");
        kprintf("V7.5 supervision skipped: no tasks\n");
        kprintf("V7.5 done\n");
        kprintf("V8.1 font skipped: no tasks\n");
        kprintf("V8.1 done\n");
        kprintf("V8.2 shaping skipped: no tasks\n");
        kprintf("V8.2 done\n");
        kprintf("V8.3 type skipped: no tasks\n");
        kprintf("V8.3 done\n");
        kprintf("V8.4 desktop skipped: no tasks\n");
        kprintf("V8.4 done\n");
        kprintf("V8.5 shell window skipped: no tasks\n");
        kprintf("V8.5 done\n");
        kprintf("V8.6 ui skipped: no tasks\n");
        kprintf("V8.6 done\n");
        kprintf("V8.7 workbench skipped: no tasks\n");
        kprintf("V8.7 done\n");
        arch_halt();
    }
    sched_idle();
    arch_halt();
}
