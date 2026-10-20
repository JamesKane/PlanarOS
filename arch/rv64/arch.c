/*
 * arch/rv64/arch.c -- RV64 in S-mode under SBI firmware
 */
#include "arch.h"
#include "fdt.h"
#include "intc.h"
#include "riscv.h"
#include "sbi.h"

const char arch_name[] = "rv64";

extern const struct intc_driver plic_driver;
const struct intc_driver *const arch_intc_drivers[] = { &plic_driver, nullptr };

extern uint64_t boot_root[512];     /* entry.S */
extern char _kernel_start[], _kernel_end[];     /* link.ld */

static bool have_sstc;              /* stimecmp works: no SBI call per timer */

void arch_early_init(void)
{
    uint64_t v;

    /* entry.S also mapped physical memory at 0 to get here; the kernel
     * now runs in the upper half, so take the low copy away. */
    for (unsigned g = 0; g < ARCH_DIRECT_MAP_GIB; g++)
        boot_root[g] = 0;
    sfence_vma_all();

    trap_init();
    /* The kernel is built without FP and vector: a stray use faults
     * instead of clobbering a task's state. */
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_FS | SSTATUS_VS_MASK) : "memory");
    /* User mode may read the clock and cycle counter without a trap. */
    __asm__ volatile("csrw scounteren, %0" : : "r"(3UL));
    have_sstc = probe_stimecmp(&v) == 0;
    /* stimecmp's reset value is unspecified: disarm before the timer
     * interrupt is enabled. Interrupts stay off until arch_irq_enable(). */
    arch_timer_arm(~(uint64_t)0);
    csr_write_sie(SIE_STIE | SIE_SEIE);
}

/* The direct map's gigapages need no memory types (the PMAs give them). */
void arch_memory_init(void)
{
}

const char *arch_privilege(void)
{
    return "S-mode";
}

void arch_kernel_extent(uint64_t *start, uint64_t *end)
{
    *start = virt_to_phys(_kernel_start);
    *end = virt_to_phys(_kernel_end);
}

uint64_t arch_timebase_hz(void)
{
    return fdt_getprop_u32(fdt_path("/cpus"), "timebase-frequency", 0);
}

/* The timer has a trap cause of its own, not an interrupt-controller source. */
uint32_t arch_timer_irq(void)
{
    return 0;
}

const char *arch_timer_name(void)
{
    return have_sstc ? "Sstc stimecmp" : "SBI set_timer";
}

void arch_timer_arm(uint64_t when)
{
    if (have_sstc)
        __asm__ volatile("csrw 0x14d, %0" : : "r"(when) : "memory");   /* stimecmp */
    else
        sbi_call(SBI_EXT_TIME, SBI_TIME_SET_TIMER, when, 0, 0);
}

void arch_irq_enable(void)
{
    __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_SIE) : "memory");
}

void arch_irq_disable(void)
{
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_SIE) : "memory");
}

unsigned long arch_irq_save(void)
{
    uint64_t s;

    __asm__ volatile("csrrc %0, sstatus, %1" : "=r"(s) : "r"(SSTATUS_SIE) : "memory");
    return (s & SSTATUS_SIE) != 0;
}

void arch_irq_restore(unsigned long was)
{
    if (was)
        arch_irq_enable();
}

void arch_wait_for_interrupt(void)
{
    __asm__ volatile("wfi" : : : "memory");
}

[[noreturn]] void arch_halt(void)
{
    __asm__ volatile("csrw sie, zero");
    for (;;)
        __asm__ volatile("wfi");
}
