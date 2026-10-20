/*
 * arch/aarch64/gicv3.c -- Arm GICv3/v4 interrupt controller
 *
 * The distributor and redistributors are MMIO; the CPU interface is
 * system registers, which is why this lives under arch/. Registered with
 * the kernel as gicv3_driver (kernel/intc.h). Everything is Group 1
 * non-secure, as seen from EL1. Interrupt level n (1-7, 7 most urgent)
 * becomes GIC priority (8 - n) << 5.
 *
 * Boards: RK3588 (GIC-600, CPU number in Aff1), CIX P1 (GICv4.1),
 * SC8280XP, and QEMU's virt machine with gic-version=3.
 */
#include <planar/arch_inline.h>
#include <planar/endian.h>

#include "aarch64.h"
#include "cpu.h"
#include "fdt.h"
#include "intc.h"

/* Distributor */
constexpr uint64_t GICD_CTLR       = 0x0000;
constexpr uint64_t GICD_TYPER      = 0x0004;
constexpr uint64_t GICD_IGROUPR    = 0x0080;
constexpr uint64_t GICD_ISENABLER  = 0x0100;
constexpr uint64_t GICD_ICENABLER  = 0x0180;
constexpr uint64_t GICD_IPRIORITYR = 0x0400;
constexpr uint64_t GICD_IROUTER    = 0x6000;
constexpr uint32_t GICD_CTLR_RWP   = 1u << 31;
constexpr uint32_t GICD_CTLR_ARE_NS = 1u << 4;
constexpr uint32_t GICD_CTLR_GRP1A = 1u << 1;

/* Redistributor: RD_base frame, then SGI_base frame 64 KiB above it. */
constexpr uint64_t GICR_TYPER      = 0x0008;
constexpr uint64_t GICR_WAKER      = 0x0014;
constexpr uint64_t GICR_SGI        = 0x10000;
constexpr uint64_t GICR_IGROUPR0   = GICR_SGI + 0x0080;
constexpr uint64_t GICR_ISENABLER0 = GICR_SGI + 0x0100;
constexpr uint64_t GICR_ICENABLER0 = GICR_SGI + 0x0180;
constexpr uint64_t GICR_IPRIORITYR = GICR_SGI + 0x0400;
constexpr uint32_t WAKER_PROCESSOR_SLEEP = 1u << 1;
constexpr uint32_t WAKER_CHILDREN_ASLEEP = 1u << 2;
constexpr uint64_t TYPER_VLPIS     = 1u << 1;
constexpr uint64_t TYPER_LAST      = 1u << 4;

constexpr uint32_t SPURIOUS        = 1020;   /* INTIDs 1020-1023 aren't interrupts */

static uintptr_t dist, rdist_base;
static uintptr_t rdists[MAX_CPUS];     /* each CPU's redistributor */

#define rdist (rdists[this_cpu()->index])
static uint32_t nlines;

static volatile uint32_t *d32(uint64_t off) { return (volatile uint32_t *)(dist + off); }
static volatile uint8_t *d8(uint64_t off) { return (volatile uint8_t *)(dist + off); }
static volatile uint64_t *d64(uint64_t off) { return (volatile uint64_t *)(dist + off); }
static volatile uint32_t *r32(uint64_t off) { return (volatile uint32_t *)(rdist + off); }
static volatile uint8_t *r8(uint64_t off) { return (volatile uint8_t *)(rdist + off); }

static uint8_t priority(unsigned level)
{
    return (uint8_t)((8 - level) << 5);
}

static void dist_wait(void)
{
    while (*d32(GICD_CTLR) & GICD_CTLR_RWP)
        ;
}

static bool probe(int32_t node)
{
    uint64_t addr, size;

    if (!fdt_reg(node, 0, &addr, &size))
        return false;
    dist = (uintptr_t)phys_to_virt(addr);
    if (!fdt_reg(node, 1, &addr, &size))
        return false;
    rdist_base = (uintptr_t)phys_to_virt(addr);

    *d32(GICD_CTLR) = 0;
    dist_wait();
    nlines = 32 * ((*d32(GICD_TYPER) & 0x1f) + 1);
    if (nlines > 1020)
        nlines = 1020;
    /* Every SPI: Group 1, masked, middling priority. Routed when enabled. */
    for (uint32_t i = 32; i < nlines; i += 32) {
        *d32(GICD_IGROUPR + i / 8) = ~0u;
        *d32(GICD_ICENABLER + i / 8) = ~0u;
    }
    for (uint32_t i = 32; i < nlines; i++)
        *d8(GICD_IPRIORITYR + i) = priority(4);
    dist_wait();
    *d32(GICD_CTLR) = GICD_CTLR_ARE_NS | GICD_CTLR_GRP1A;
    dist_wait();
    return true;
}

/* The redistributor whose affinity matches 'cpu' (Aff2.Aff1.Aff0). */
static uintptr_t find_rdist(uint64_t cpu)
{
    uintptr_t r = rdist_base;

    for (;;) {
        uint64_t typer = *(volatile uint64_t *)(r + GICR_TYPER);

        if ((typer >> 32) == cpu)
            return r;
        if (typer & TYPER_LAST)
            return 0;
        r += typer & TYPER_VLPIS ? 0x40000 : 0x20000;
    }
}

static bool init_cpu(uint64_t cpu)
{
    if (!(rdists[this_cpu()->index] = find_rdist(cpu)))
        return false;
    *r32(GICR_WAKER) &= ~WAKER_PROCESSOR_SLEEP;
    while (*r32(GICR_WAKER) & WAKER_CHILDREN_ASLEEP)
        ;
    /* SGIs and PPIs: Group 1, masked, middling priority. */
    *r32(GICR_IGROUPR0) = ~0u;
    *r32(GICR_ICENABLER0) = ~0u;
    for (uint32_t i = 0; i < 32; i++)
        *r8(GICR_IPRIORITYR + i) = priority(4);

    /* The CPU interface: system registers, all priorities, Group 1 on. */
    sysreg_write(ICC_SRE_EL1, sysreg_read(ICC_SRE_EL1) | 1);
    isb();
    sysreg_write(ICC_PMR_EL1, 0xff);
    sysreg_write(ICC_BPR1_EL1, 0);
    sysreg_write(ICC_CTLR_EL1, 0);          /* EOI drops priority and deactivates */
    sysreg_write(ICC_IGRPEN1_EL1, 1);
    isb();
    return true;
}

static void set(uint32_t source, unsigned level)
{
    uint32_t bit = 1u << (source % 32);

    if (source >= nlines)
        return;
    if (source < 32) {
        if (level) {
            *r8(GICR_IPRIORITYR + source) = priority(level);
            *r32(GICR_ISENABLER0) = bit;
        } else {
            *r32(GICR_ICENABLER0) = bit;
        }
        return;
    }
    if (level) {
        *d8(GICD_IPRIORITYR + source) = priority(level);
        /* Route to the calling CPU. */
        *d64(GICD_IROUTER + 8 * (uint64_t)source) = sysreg_read(mpidr_el1) & 0xff00ffffffULL;
        *d32(GICD_ISENABLER + source / 32 * 4) = bit;
    } else {
        *d32(GICD_ICENABLER + source / 32 * 4) = bit;
        dist_wait();
    }
}

static uint32_t claim(void)
{
    uint32_t intid = (uint32_t)sysreg_read(ICC_IAR1_EL1) & 0xffffff;

    return intid >= SPURIOUS && intid <= 1023 ? 0 : intid;
}

static void complete(uint32_t source)
{
    sysreg_write(ICC_EOIR1_EL1, source);
    isb();
}

/* Three cells per interrupt: type (0 SPI, 1 PPI), number, flags. */
static uint32_t fdt_irq(int32_t node, uint32_t index)
{
    uint32_t len;
    const uint8_t *p = fdt_getprop(node, "interrupts", &len);

    if (!p || (index + 1) * 12 > len)
        return 0;
    p += 12 * index;
    return get_be32(p + 4) + (get_be32(p) == 1 ? 16 : 32);
}

static const char *const compatible[] = { "arm,gic-v3", nullptr };

const struct intc_driver gicv3_driver = {
    .name = "GICv3",
    .compatible = compatible,
    .probe = probe,
    .init_cpu = init_cpu,
    .set = set,
    .claim = claim,
    .complete = complete,
    .fdt_irq = fdt_irq,
};
