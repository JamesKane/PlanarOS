/*
 * arch/aarch64/arch.c -- AArch64 at EL1
 */
#include "aarch64.h"
#include "arch.h"
#include "cpu.h"
#include "fdt.h"
#include "intc.h"
#include "kprintf.h"
#include "memmap.h"

const char arch_name[] = "aarch64";

extern const struct intc_driver gicv3_driver;
const struct intc_driver *const arch_intc_drivers[] = { &gicv3_driver, nullptr };

extern const char vectors[];                    /* vectors.S */
extern const char secondary_entry[];            /* entry.S */
extern uint64_t boot_l1[512], boot_l2[512];     /* entry.S */
extern char _kernel_start[], _kernel_end[];     /* link.ld */

constexpr uint64_t DESC_VALID_MASK = 3;
constexpr uint64_t DESC_BLOCK      = 1;
constexpr uint64_t DESC_TABLE      = 3;
constexpr uint64_t BLOCK_NORMAL    = 0x004000000000070d;     /* UXN: never user code */
constexpr uint64_t GIB             = 1ULL << 30;
constexpr uint64_t MIB2            = 1ULL << 21;

/* Level-2 tables for GiBs that hold both RAM and devices. */
constexpr int L2_POOL = 8;
alignas(4096) static uint64_t l2_pool[L2_POOL][512];
static int l2_used;

static bool have_pmu;

/* The PSCI conduit, from the device tree: hvc or smc. */
static bool psci_smc;

constexpr uint32_t IPI_SGI = 1;

void arch_early_init(void)
{
    arch_cpu_init();
}

/* Every CPU: vectors, TTBR0 off, the cycle counter, the virtual timer. */
void arch_cpu_init(void)
{
    uint64_t dfr0;

    sysreg_write(vbar_el1, (uintptr_t)vectors);
    isb();
    /* entry.S also mapped physical memory through TTBR0 to get here; the
     * kernel now runs in TTBR1's half, and TTBR0 belongs to user domains:
     * the kernel's own is an empty table. */
    arch_domain_switch(nullptr);
    tlb_flush_local();
    __asm__ volatile("msr pan, #1" : : : "memory");
    /* User mode may read the virtual counter (TimeNow without a trap). */
    sysreg_write(cntkctl_el1, sysreg_read(cntkctl_el1) | 2);

    /* The cycle counter, if there's a PMU (PMUVer neither 0 nor 0xf). */
    dfr0 = (sysreg_read(id_aa64dfr0_el1) >> 8) & 0xf;
    have_pmu = dfr0 != 0 && dfr0 != 0xf;
    if (have_pmu) {
        sysreg_write(pmcr_el0, sysreg_read(pmcr_el0) | 1);     /* E */
        sysreg_write(pmcntenset_el0, 1ULL << 31);              /* PMCCNTR */
        sysreg_write(pmuserenr_el0, 4);     /* CR alone: user mode reads the cycle
                                             * counter, controls nothing (review M2) */
    }

    /* FP/SIMD traps until a task's state is loaded (lazy.c). */
    /* The whole register, not what the firmware left: FP/SIMD, SVE and SME
     * trap (lazy.c turns FP/SIMD on per task), trace doesn't (review M1). */
    sysreg_write(cpacr_el1, 0);
    isb();

    /* The virtual timer: on, unmasked, disarmed. */
    sysreg_write(cntv_cval_el0, ~(uint64_t)0);
    sysreg_write(cntv_ctl_el0, 1);
    isb();
}

void arch_set_this_cpu(struct cpu *c)
{
    sysreg_write(tpidr_el1, (uintptr_t)c);
}

static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;

    if (psci_smc)
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}

constexpr uint64_t PSCI_CPU_ON = 0xc4000003;    /* SMC64 */

/*
 * The conduit comes from the device tree, so it's read here, not in
 * arch_early_init(), which runs before the tree is. Getting it wrong
 * isn't benign: from EL1, hvc goes to whatever EL2 has (nothing, once
 * the entry code has dropped from it), and smc to a firmware that may
 * not be there.
 */
static bool psci_ready(void)
{
    static bool conduit_known;

    if (!conduit_known) {
        int32_t psci = fdt_find_compatible(-1, "arm,psci-0.2");
        uint32_t len;
        const char *method;

        if (psci < 0)
            psci = fdt_find_compatible(-1, "arm,psci-1.0");
        method = psci >= 0 ? fdt_getprop(psci, "method", &len) : nullptr;
        if (!method)
            return false;
        psci_smc = method[0] == 's';
        conduit_known = true;
    }
    return true;
}

bool arch_cpu_start(struct cpu *c)
{
    if (!psci_ready())
        return false;
    return psci_call(PSCI_CPU_ON, c->hwid, virt_to_phys(secondary_entry),
                     (uint64_t)(uintptr_t)c) == 0;
}

/* SGI to one CPU by affinity: Aff2 and Aff1 select the cluster, then
 * RS and TargetList pick Aff0 in groups of 16. */
void arch_send_ipi(struct cpu *c)
{
    uint64_t aff0 = c->hwid & 0xff, aff1 = (c->hwid >> 8) & 0xff, aff2 = (c->hwid >> 16) & 0xff;
    uint64_t sgi = aff2 << 32 | (aff0 / 16) << 44 | (uint64_t)IPI_SGI << 24 |
                   aff1 << 16 | 1ULL << (aff0 % 16);

    /* The call record must be visible before the interrupt lands. */
    __asm__ volatile("dsb ishst" : : : "memory");
    sysreg_write(ICC_SGI1R_EL1, sgi);
    isb();
}

uint32_t arch_ipi_irq(void)
{
    return IPI_SGI;
}

extern const char arch_task_start[];   /* switch.S */

void arch_context_init(struct arch_context *ctx, void *stack_top)
{
    *ctx = (struct arch_context){
        .lr = (uint64_t)(uintptr_t)arch_task_start,
        .sp = (uint64_t)(uintptr_t)stack_top & ~(uint64_t)15,
        .lazy_cpu = -1,
    };
}

/* Break-before-make: replace a live descriptor. */
static void set_desc(uint64_t *slot, uint64_t desc)
{
    *slot = 0;
    tlb_flush_local();
    *slot = desc;
    tlb_flush_local();
}

/* The level-2 table under L1 entry 'g', splitting a 1 GiB block if
 * needed; nullptr if the pool is used up. */
static uint64_t *l2_for(unsigned g)
{
    uint64_t *t;

    if ((boot_l1[g] & DESC_VALID_MASK) == DESC_TABLE)
        return phys_to_virt(boot_l1[g] & 0x0000fffffffff000ULL);
    if (l2_used == L2_POOL)
        return nullptr;
    t = l2_pool[l2_used++];
    for (unsigned i = 0; i < 512; i++)
        t[i] = ((uint64_t)g * GIB + i * MIB2) | (boot_l1[g] & ~0x0000ffffffe00000ULL);
    set_desc(&boot_l1[g], virt_to_phys(t) | DESC_TABLE);
    return t;
}

/*
 * Make every RAM range normal write-back memory: whole GiBs as 1 GiB
 * blocks, the rest as 2 MiB blocks. memmap_init() rounds RAM to 2 MiB.
 * The GiB holding the kernel is already a table (entry.S), so the blocks
 * the kernel runs from never change.
 */
void arch_memory_init(void)
{
    uint64_t base, size;

    for (unsigned i = 0; memmap_ram(i, &base, &size); i++) {
        uint64_t end = base + size;

        for (uint64_t pa = base; pa < end; ) {
            unsigned g = (unsigned)(pa / GIB);
            uint64_t *l2;

            if (g >= ARCH_DIRECT_MAP_GIB)
                break;              /* memmap_init clipped RAM here already */
            if (pa % GIB == 0 && end - pa >= GIB &&
                (boot_l1[g] & DESC_VALID_MASK) == DESC_BLOCK) {
                set_desc(&boot_l1[g], pa | BLOCK_NORMAL);
                pa += GIB;
                continue;
            }
            if (!(l2 = l2_for(g))) {
                kprintf("planar: out of level-2 tables; RAM from %p left unmapped\n",
                        (void *)pa);
                memmap_clip(pa);    /* and out of the page allocator's way */
                return;
            }
            for (; pa < end && pa / GIB == g; pa += MIB2) {
                uint64_t *slot = &l2[(pa / MIB2) % 512];

                if (*slot != (pa | BLOCK_NORMAL))
                    set_desc(slot, pa | BLOCK_NORMAL);
            }
        }
    }
}

const char *arch_privilege(void)
{
    switch ((sysreg_read(CurrentEL) >> 2) & 3) {
    case 1:  return "EL1";
    case 2:  return "EL2";
    default: return "EL?";
    }
}

void arch_kernel_extent(uint64_t *start, uint64_t *end)
{
    *start = virt_to_phys(_kernel_start);
    *end = virt_to_phys(_kernel_end);
}

uint64_t arch_timebase_hz(void)
{
    return sysreg_read(cntfrq_el0);
}

/* The virtual timer's PPI: the third interrupt of the "arm,armv8-timer"
 * node (secure, non-secure physical, virtual, hypervisor); 27 by the
 * architecture's recommendation if the node is missing. */
uint32_t arch_timer_irq(void)
{
    int32_t node = fdt_find_compatible(-1, "arm,armv8-timer");
    uint32_t irq = node >= 0 ? intc_fdt_irq(node, 2) : 0;

    return irq ? irq : 27;
}

const char *arch_timer_name(void)
{
    return "generic timer (virtual)";
}

void arch_timer_arm(uint64_t when)
{
    sysreg_write(cntv_cval_el0, when);
    isb();
}

void arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" : : : "memory");
}

void arch_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" : : : "memory");
}

unsigned long arch_irq_save(void)
{
    uint64_t daif = sysreg_read(daif);

    arch_irq_disable();
    return !(daif & (1u << 7));     /* I clear: interrupts were on */
}

void arch_irq_restore(unsigned long was)
{
    if (was)
        arch_irq_enable();
}

void arch_wait_for_interrupt(void)
{
    __asm__ volatile("dsb sy\n\twfi" : : : "memory");
}

[[noreturn]] void arch_reboot(void)
{
    constexpr uint64_t PSCI_SYSTEM_RESET = 0x84000009;

    if (psci_ready())
        psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
    kprintf("planar: reboot failed: PSCI SYSTEM_RESET returned\n");
    arch_halt();
}

[[noreturn]] void arch_halt(void)
{
    __asm__ volatile("msr daifset, #0xf");
    for (;;)
        __asm__ volatile("wfi");
}
