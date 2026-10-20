/*
 * arch/rv64/arch.c -- RV64 in S-mode under SBI firmware
 */
#include "arch.h"
#include "cpu.h"
#include "fdt.h"
#include "intc.h"
#include "kprintf.h"
#include "pmm.h"
#include "riscv.h"
#include "sbi.h"

const char arch_name[] = "rv64";

extern const struct intc_driver plic_driver;
const struct intc_driver *const arch_intc_drivers[] = { &plic_driver, nullptr };

extern uint64_t boot_root[512];     /* entry.S */
extern uint64_t secondary_satp, kernel_satp;
extern const char secondary_entry[];

constexpr uint64_t SATP_SV39 = 8ULL << 60;
constexpr uint64_t PTE_KERNEL = 0xef;   /* V R W X G A D, as entry.S */
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

    /* The Sstc probe may trap, and arch_cpu_init() needs its answer. */
    trap_init();
    have_sstc = probe_stimecmp(&v) == 0;
    kernel_satp = csr_read_satp();
    riscv_probe_asids();
    __asm__ volatile("csrs sstatus, %0" : : "r"(2ULL << 9) : "memory");     /* VS clean */
    riscv_lazy_init(probe_vlenb(&v) == 0 ? v : 0);
    arch_cpu_init();
}

/* Every CPU: traps; no FP or vector in the kernel, so a stray use faults
 * instead of clobbering a task's state; the clock and cycle counter for
 * user mode; the timer disarmed; and the interrupts the kernel takes. */
void arch_cpu_init(void)
{
    trap_init();
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_FS | SSTATUS_VS_MASK) : "memory");
    __asm__ volatile("csrw scounteren, %0" : : "r"(3UL));
    /* stimecmp's reset value is unspecified: disarm before the timer
     * interrupt is enabled. Interrupts stay off until arch_irq_enable(). */
    arch_timer_arm(~(uint64_t)0);
    csr_write_sie(SIE_SSIE | SIE_STIE | SIE_SEIE);
}

void arch_set_this_cpu(struct cpu *c)
{
    __asm__ volatile("mv tp, %0" : : "r"(c) : "memory");
}

/*
 * Start hart c->hwid at secondary_entry. The first time, build the table
 * it turns its MMU on with: the kernel's upper half, plus physical memory
 * mapped at 0 as entry.S did for the boot hart.
 */
bool arch_cpu_start(struct cpu *c)
{
    struct sbiret r;

    if (!secondary_satp) {
        uint64_t root = pmm_alloc();
        uint64_t *t = phys_to_virt(root);

        if (!root)
            return false;
        for (unsigned i = 256; i < 512; i++)
            t[i] = boot_root[i];
        for (uint64_t g = 0; g < ARCH_DIRECT_MAP_GIB; g++)
            t[g] = g << 28 | PTE_KERNEL;
        secondary_satp = SATP_SV39 | root >> 12;
    }
    r = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_START, c->hwid,
                 virt_to_phys(secondary_entry), (uint64_t)(uintptr_t)c);
    return r.error == 0;
}

/* The calling hart's marchid; 0 (not implemented) if the call fails. */
uint64_t arch_cpu_type(void)
{
    struct sbiret r = sbi_call(SBI_EXT_BASE, SBI_BASE_GET_MARCHID, 0, 0, 0);

    return r.error ? 0 : r.value;
}

const char *arch_cpu_type_name(uint64_t)
{
    return nullptr;
}

void arch_send_ipi(struct cpu *c)
{
    /* The call record must be visible before the interrupt lands. */
    __asm__ volatile("fence rw, rw" : : : "memory");
    sbi_call(SBI_EXT_IPI, SBI_IPI_SEND_IPI, 1, c->hwid, 0);
}

/* IPIs are the supervisor software interrupt, a trap cause of their own. */
uint32_t arch_ipi_irq(void)
{
    return 0;
}

extern const char arch_task_start[];   /* switch.S */

void arch_context_init(struct arch_context *ctx, void *stack_top)
{
    *ctx = (struct arch_context){
        .ra = (uint64_t)(uintptr_t)arch_task_start,
        .sp = (uint64_t)(uintptr_t)stack_top & ~(uint64_t)15,
        .lazy_cpu = -1,
    };
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

[[noreturn]] void arch_reboot(void)
{
    struct sbiret r;

    if (!sbi_has(SBI_EXT_SRST)) {
        kprintf("planar: reboot failed: the firmware has no SBI SRST\n");
        arch_halt();
    }
    r = sbi_call(SBI_EXT_SRST, SBI_SRST_RESET, 1, 0, 0);    /* cold reboot */
    kprintf("planar: reboot failed: SBI SRST returned %lld\n", (long long)r.error);
    arch_halt();
}

[[noreturn]] void arch_halt(void)
{
    __asm__ volatile("csrw sie, zero");
    for (;;)
        __asm__ volatile("wfi");
}

/*
 * TLB counters, through the SBI PMU: the firmware owns the event
 * selectors (M-mode CSRs), so the kernel asks it for counters of the
 * standard cache events, then reads them as hpmcounters. Which of the
 * hart's own events those are is the firmware's business. Leaving out
 * S- and M-mode needs Sscofpmf; without it they count too.
 */
#define CACHE_EVENT(id, op, miss)   (1u << 16 | (id) << 3 | (op) << 1 | (miss))

static const struct {
    uint32_t event;
    const char *name;
} tlb_events[] = {
    { CACHE_EVENT(3, 0, 1), "DTLB read misses" },
    { CACHE_EVENT(3, 1, 1), "DTLB write misses" },
    { CACHE_EVENT(4, 0, 1), "ITLB misses" },
};

constexpr uint64_t PMU_CLEAR_VALUE = 1u << 1, PMU_SINH = 1u << 4, PMU_MINH = 1u << 6;
constexpr uint64_t PMU_START_SET_INIT = 1u << 0;

/* Per CPU: the counters configured, and their CSRs (0: none). */
static struct {
    uint32_t idx[ARCH_TLB_EVENTS], csr[ARCH_TLB_EVENTS];
    const char *name[ARCH_TLB_EVENTS];
    unsigned n;
    bool tried;
} tlb[MAX_CPUS];

/* Read hpmcounter 'csr' (0xc03-0xc1f); csrr needs the number fixed. */
static uint64_t read_hpm(uint32_t csr)
{
    uint64_t v = 0;

#define CASE(n) case 0xc00 + n: __asm__ volatile("csrr %0, %1" : "=r"(v) : "i"(0xc00 + n)); break;
    switch (csr) {
    CASE(3) CASE(4) CASE(5) CASE(6) CASE(7) CASE(8) CASE(9) CASE(10)
    CASE(11) CASE(12) CASE(13) CASE(14) CASE(15) CASE(16) CASE(17) CASE(18)
    CASE(19) CASE(20) CASE(21) CASE(22) CASE(23) CASE(24) CASE(25) CASE(26)
    CASE(27) CASE(28) CASE(29) CASE(30) CASE(31)
    default: break;
    }
#undef CASE
    return v;
}

/* The first time on a CPU: find a counter for each event the firmware
 * can count, and keep it. */
static void tlb_configure(unsigned cpu)
{
    struct sbiret n, r, info;
    uint64_t mask;

    tlb[cpu].tried = true;
    if (!sbi_has(SBI_EXT_PMU))
        return;
    n = sbi_call(SBI_EXT_PMU, SBI_PMU_NUM_COUNTERS, 0, 0, 0);
    mask = n.value >= 64 ? ~0ULL : (1ULL << n.value) - 1;
    for (unsigned e = 0; e < sizeof tlb_events / sizeof tlb_events[0]; e++) {
        unsigned k = tlb[cpu].n;

        if (k == ARCH_TLB_EVENTS)
            break;
        r = sbi_call5(SBI_EXT_PMU, SBI_PMU_COUNTER_CFG_MATCH, 0, mask,
                      PMU_CLEAR_VALUE | PMU_SINH | PMU_MINH, tlb_events[e].event, 0);
        if (r.error)
            continue;
        info = sbi_call(SBI_EXT_PMU, SBI_PMU_COUNTER_GET_INFO, r.value, 0, 0);
        /* Firmware counters (the top bit) aren't CSRs. */
        if (info.error || info.value >> 63 || (info.value & 0xfff) < 0xc03 ||
            (info.value & 0xfff) > 0xc1f)
            continue;
        mask &= ~(1ULL << r.value);         /* not again for the next event */
        tlb[cpu].idx[k] = (uint32_t)r.value;
        tlb[cpu].csr[k] = (uint32_t)(info.value & 0xfff);
        tlb[cpu].name[k] = tlb_events[e].name;
        tlb[cpu].n++;
    }
}

unsigned arch_tlb_count_start(const char *names[ARCH_TLB_EVENTS])
{
    unsigned cpu = this_cpu()->index;

    if (!tlb[cpu].tried)
        tlb_configure(cpu);
    for (unsigned k = 0; k < tlb[cpu].n; k++) {
        sbi_call(SBI_EXT_PMU, SBI_PMU_COUNTER_STOP, tlb[cpu].idx[k], 1, 0);
        sbi_call5(SBI_EXT_PMU, SBI_PMU_COUNTER_START, tlb[cpu].idx[k], 1, PMU_START_SET_INIT,
                  0, 0);
        names[k] = tlb[cpu].name[k];
    }
    return tlb[cpu].n;
}

void arch_tlb_count_read(unsigned n, uint64_t counts[ARCH_TLB_EVENTS])
{
    unsigned cpu = this_cpu()->index;

    for (unsigned k = 0; k < n && k < tlb[cpu].n; k++)
        counts[k] = read_hpm(tlb[cpu].csr[k]);
}
