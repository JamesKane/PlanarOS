/*
 * arch/aarch64/its.c -- the GICv3 Interrupt Translation Service, and LPIs
 * (Arm IHI 0069H, §5 and §6)
 *
 * PCI functions signal by MSI: a write of an event ID to the ITS's
 * GITS_TRANSLATER, which the ITS tags with the writer's device ID and
 * translates, through tables it keeps in memory, to an LPI on some CPU.
 * The kernel owns the ITS (V4 step 5); hwdb asks it for a device's
 * LPIs (AllocMsi), which it maps event by event, and leases them to the
 * device's driver as lines like any other.
 *
 * - One property table for every redistributor: a byte per LPI, its
 *   priority and whether it's enabled. LPI_IDBITS bits of INTID, so LPIs
 *   8192 to 8192 + LPIS - 1.
 * - A pending table per redistributor, 64 KiB aligned.
 * - The ITS's device table, two-level when the ITS can (a page of
 *   entries made at MAPD), and its collection table if it doesn't keep
 *   collections itself. Collection n targets CPU n.
 * - An ITT per device, as big as its events need.
 * - The command queue, 64 KiB, each command waited on with SYNC: they
 *   are rare (a device's setup, an LPI enabled, moved or masked).
 *
 * When the ITS says its tables aren't shareable, they're kept
 * non-cacheable, and what the kernel writes is cleaned to the point of
 * coherency. LPIs are edge-triggered: nothing masks one as it fires.
 */
#include <planar/arch_inline.h>

#include "aarch64.h"
#include "acpi.h"
#include "cpu.h"
#include "devmem.h"
#include "fdt.h"
#include "irq.h"
#include "its.h"
#include "kprintf.h"
#include "kstring.h"
#include "pmm.h"
#include "spinlock.h"

constexpr uint64_t GITS_CTLR = 0x0000;
constexpr uint64_t GITS_TYPER = 0x0008;
constexpr uint64_t GITS_CBASER = 0x0080;
constexpr uint64_t GITS_CWRITER = 0x0088;
constexpr uint64_t GITS_CREADR = 0x0090;
constexpr uint64_t GITS_BASER = 0x0100;
constexpr uint64_t GITS_TRANSLATER = 0x10040;

constexpr uint64_t GICR_CTLR = 0x0000;
constexpr uint64_t GICR_TYPER = 0x0008;
constexpr uint64_t GICR_PROPBASER = 0x0070;
constexpr uint64_t GICR_PENDBASER = 0x0078;

constexpr unsigned LPI_IDBITS = 14;             /* INTIDs below 16384 */
constexpr uint32_t LPI_BASE = 8192;
constexpr uint32_t LPI_TABLE = (1u << LPI_IDBITS) - LPI_BASE;   /* the property table's */
constexpr uint32_t LPIS = IRQ_SOURCES - LPI_BASE;               /* those handed out */
constexpr uint64_t CMDQ_BYTES = 64 * 1024;
constexpr uint64_t PAGE = 4096;
constexpr unsigned MAX_DEVICES = 64;

/* Cacheability and shareability, as the BASER-style registers spell them. */
constexpr uint64_t RAWAWB = 7, NC = 1, INNER_SHAREABLE = 1;
constexpr uint64_t BASER_VALID = 1ULL << 63, BASER_INDIRECT = 1ULL << 62;

enum : uint8_t {
    CMD_MOVI = 0x01, CMD_SYNC = 0x05, CMD_MAPD = 0x08, CMD_MAPC = 0x09, CMD_MAPTI = 0x0a,
    CMD_INV = 0x0c,
};

static uintptr_t its;
static uint64_t its_pa, typer;
static bool noncoherent;            /* the ITS doesn't snoop: clean what's written */
static spinlock_t lock;

static uint8_t *prop;               /* the property table */
static uint64_t prop_pa;
static uint64_t *cmdq;
static uint64_t cmdq_pa, cwriter;
static uint64_t *devtab;            /* the device table, or its first level */
static bool indirect;
static uint64_t dev_esz, dev_entries;   /* bytes per entry; entries (first level: pointers) */
static unsigned itt_esz;

static uint64_t rdbase[MAX_CPUS];   /* each CPU's, as MAPC and SYNC name it */
static bool cpu_ready[MAX_CPUS];

/* Which device and event each LPI is, and the collection it's on. */
static struct { uint32_t dev, event; uint16_t icid; bool mapped; } lpi[LPIS];
static uint32_t next_lpi;
static uint64_t kept;               /* pages made for devices: theirs for the boot */
static struct { uint32_t id, first, count; } devices[MAX_DEVICES];
static unsigned ndevices;

static_assert(IRQ_SOURCES > LPI_BASE && IRQ_SOURCES <= 1u << LPI_IDBITS, "LPIs are sources");

static uint8_t priority_byte(unsigned level)
{
    return (uint8_t)(((8 - level) << 5) & 0xfc);
}

static volatile uint64_t *g64(uint64_t off) { return (volatile uint64_t *)(its + off); }
static volatile uint32_t *g32(uint64_t off) { return (volatile uint32_t *)(its + off); }

static void clean(const void *p, uint64_t len)
{
    if (!noncoherent)
        return;
    for (uint64_t a = (uint64_t)p & ~63ULL; a < (uint64_t)p + len; a += 64)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

/* --- Commands. --- */

/* Queue one command and wait until the ITS has read it. Lock held. */
static void command(uint64_t d0, uint64_t d1, uint64_t d2, uint64_t d3)
{
    uint64_t *c = (uint64_t *)((uint8_t *)cmdq + cwriter);

    c[0] = d0;
    c[1] = d1;
    c[2] = d2;
    c[3] = d3;
    clean(c, 32);
    __asm__ volatile("dsb ishst" : : : "memory");
    cwriter = (cwriter + 32) % CMDQ_BYTES;
    *g64(GITS_CWRITER) = cwriter;
    while ((*g64(GITS_CREADR) & (CMDQ_BYTES - 1)) != cwriter)
        arch_cpu_relax();
}

static void sync_cmd(unsigned cpu)
{
    command(CMD_SYNC, 0, rdbase[cpu] << 16, 0);
}

/* --- Tables. --- */

/* Program GITS_BASER<n> for 'type' (1 devices, 4 collections) with a
 * table for 'ids' IDs; false if it can't be made. */
static bool baser(unsigned n, unsigned type, uint64_t ids)
{
    uint64_t v = *g64(GITS_BASER + 8 * n), esz = (v >> 48 & 0x1f) + 1, pages, pa, got;
    bool two = false;

    (void)type;
    pages = (ids * esz + PAGE - 1) / PAGE;
    if (type == 1) {
        /* Try two levels: a first page of pointers to pages of entries. */
        uint64_t per = PAGE / esz, l1 = ((ids + per - 1) / per) * 8;

        *g64(GITS_BASER + 8 * n) = BASER_INDIRECT;
        two = *g64(GITS_BASER + 8 * n) & BASER_INDIRECT;
        if (two)
            pages = (l1 + PAGE - 1) / PAGE;
    }
    if (pages > 256)
        pages = 256;                /* the Size field's most, and plenty here */
    if (!(pa = pmm_alloc_contig_aligned(pages, 64 * 1024)))
        return false;
    v = BASER_VALID | (two ? BASER_INDIRECT : 0) | RAWAWB << 59 | pa | INNER_SHAREABLE << 10 |
        (pages - 1);
    *g64(GITS_BASER + 8 * n) = v;
    got = *g64(GITS_BASER + 8 * n);
    if (!(got >> 10 & 3)) {         /* not shareable: then not cached either */
        noncoherent = true;
        v = (v & ~(7ULL << 59) & ~(3ULL << 10)) | NC << 59;
        *g64(GITS_BASER + 8 * n) = v;
        got = *g64(GITS_BASER + 8 * n);
    }
    if ((got & 0xfffffffff000ULL) != pa || !(got & BASER_VALID)) {
        kprintf("planar: ITS: BASER%u (type %u) wouldn't take its table\n", n, type);
        return false;
    }
    clean(phys_to_virt(pa), pages * PAGE);
    if (type == 1) {
        devtab = phys_to_virt(pa);
        indirect = two;
        dev_esz = esz;
        dev_entries = pages * PAGE / (two ? 8 : esz);
    }
    return true;
}

/* The device table has room for 'id': its second-level page made if
 * need be. Lock held. */
static bool dev_room(uint32_t id)
{
    uint64_t per = PAGE / dev_esz, i;

    if (!indirect)
        return id < dev_entries;
    if ((i = id / per) >= dev_entries)
        return false;
    if (!(devtab[i] & BASER_VALID)) {
        uint64_t pa = pmm_alloc();

        if (!pa)
            return false;
        kept++;
        clean(phys_to_virt(pa), PAGE);
        devtab[i] = BASER_VALID | pa;
        clean(&devtab[i], 8);
    }
    return true;
}

/* --- Setup. --- */

static bool find(void)
{
    struct acpi_gic g;
    int32_t node;
    uint64_t size;

    if ((node = fdt_find_compatible(-1, "arm,gic-v3-its")) >= 0)
        return fdt_reg(node, 0, &its_pa, &size);
    if (acpi_gic(&g) && g.nits)
        return (its_pa = g.its[0]) != 0;
    return false;
}

static bool setup(void)
{
    if (!find())
        return false;
    its = (uintptr_t)phys_to_virt(its_pa);
    devmem_claim(its_pa, 0x20000);
    typer = *g64(GITS_TYPER);
    if (!(typer & 1))               /* Physical LPIs */
        return false;
    itt_esz = (unsigned)(typer >> 4 & 0xf) + 1;
    *g32(GITS_CTLR) = 0;
    while (!(*g32(GITS_CTLR) & (1u << 31)))     /* Quiescent */
        arch_cpu_relax();

    /* The tables the ITS asks for, by type: devices, and collections
     * unless it holds enough itself (HCC). */
    for (unsigned n = 0; n < 8; n++) {
        uint64_t v = *g64(GITS_BASER + 8 * n);
        unsigned type = (unsigned)(v >> 56 & 7);
        uint64_t devbits = (typer >> 13 & 0x1f) + 1;

        if (type == 1 && !baser(n, 1, 1ULL << (devbits > 20 ? 20 : devbits)))
            return false;
        if (type == 4 && (typer >> 24 & 0xff) < MAX_CPUS && !baser(n, 4, MAX_CPUS))
            return false;
    }
    if (!devtab)
        return false;

    if (!(cmdq_pa = pmm_alloc_contig_aligned(CMDQ_BYTES / PAGE, 64 * 1024)))
        return false;
    cmdq = phys_to_virt(cmdq_pa);
    *g64(GITS_CBASER) = BASER_VALID | (noncoherent ? NC : RAWAWB) << 59 | cmdq_pa |
                        (noncoherent ? 0 : INNER_SHAREABLE << 10) | (CMDQ_BYTES / PAGE - 1);
    if (*g64(GITS_CBASER) >> 10 & 3 ? false : !noncoherent) {
        noncoherent = true;
        *g64(GITS_CBASER) = BASER_VALID | NC << 59 | cmdq_pa | (CMDQ_BYTES / PAGE - 1);
    }
    cwriter = 0;
    *g64(GITS_CWRITER) = 0;

    /* The LPIs' properties: all disabled until asked for. */
    if (!(prop_pa = pmm_alloc_contig_aligned(LPI_TABLE / PAGE, 64 * 1024)))
        return false;
    prop = phys_to_virt(prop_pa);
    memset(prop, priority_byte(4), LPI_TABLE);
    clean(prop, LPI_TABLE);
    *g32(GITS_CTLR) = 1;
    return true;
}

bool its_init(void)
{
    if (setup())
        return true;
    its = 0;                        /* half set up: not used */
    return false;
}

/* The calling CPU's redistributor at 'rd': LPIs on, and its collection. */
void its_init_cpu(uintptr_t rd, uint64_t rd_pa)
{
    unsigned cpu = this_cpu()->index;
    volatile uint64_t *r64 = (volatile uint64_t *)rd;
    uint64_t pend;

    if (!its || !(r64[GICR_TYPER / 8] & 1))     /* no LPIs here */
        return;
    if (*(volatile uint32_t *)(rd + GICR_CTLR) & 1) {
        kprintf("planar: ITS: CPU %u's LPIs were left on; it takes no MSIs\n", cpu);
        return;
    }
    if (!(pend = pmm_alloc_contig_aligned((1u << LPI_IDBITS) / 8 / PAGE + 1, 64 * 1024)))
        return;
    clean(phys_to_virt(pend), (1u << LPI_IDBITS) / 8);
    r64[GICR_PROPBASER / 8] = prop_pa | (noncoherent ? NC : RAWAWB) << 7 |
                              (noncoherent ? 0 : INNER_SHAREABLE << 10) | (LPI_IDBITS - 1);
    r64[GICR_PENDBASER / 8] = 1ULL << 62 | pend | (noncoherent ? NC : RAWAWB) << 7 |
                              (noncoherent ? 0 : INNER_SHAREABLE << 10);
    __asm__ volatile("dsb sy" : : : "memory");
    *(volatile uint32_t *)(rd + GICR_CTLR) |= 1;
    __asm__ volatile("dsb sy" : : : "memory");

    rdbase[cpu] = typer >> 19 & 1 ? rd_pa >> 16 : (r64[GICR_TYPER / 8] >> 8 & 0xffff);
    unsigned long was = spin_lock_irqsave(&lock);
    command(CMD_MAPC, 0, 1ULL << 63 | rdbase[cpu] << 16 | cpu, 0);
    sync_cmd(cpu);
    cpu_ready[cpu] = true;
    spin_unlock_irqrestore(&lock, was);
}

/* --- MSIs. --- */

bool its_msi_alloc(uint32_t dev, uint32_t n, uint32_t *first, uint64_t *doorbell)
{
    unsigned bits = 1;
    uint64_t itt;
    bool ok = false;

    if (!its || n == 0 || n > 32 || !cpu_ready[0])
        return false;
    while ((1u << bits) < n)
        bits++;
    unsigned long was = spin_lock_irqsave(&lock);
    for (unsigned i = 0; i < ndevices; i++)
        if (devices[i].id == dev)
            goto out;               /* mapped already: ITTs aren't remade */
    if (ndevices == MAX_DEVICES || next_lpi + n > LPIS || !dev_room(dev) ||
        dev >> ((typer >> 13 & 0x1f) + 1))
        goto out;
    /* An ITT: 256-byte aligned; a page is enough for 32 events. */
    if (!(itt = pmm_alloc()))
        goto out;
    kept++;
    clean(phys_to_virt(itt), PAGE);
    command(CMD_MAPD | (uint64_t)dev << 32, bits - 1, 1ULL << 63 | itt, 0);
    for (uint32_t e = 0; e < n; e++) {
        uint32_t id = next_lpi + e;

        lpi[id] = (typeof(lpi[0])){ .dev = dev, .event = e, .icid = 0, .mapped = true };
        command(CMD_MAPTI | (uint64_t)dev << 32, e | (uint64_t)(LPI_BASE + id) << 32, 0, 0);
    }
    sync_cmd(0);
    devices[ndevices++] = (typeof(devices[0])){ dev, next_lpi, n };
    *first = LPI_BASE + next_lpi;
    *doorbell = its_pa + GITS_TRANSLATER;
    next_lpi += n;
    ok = true;
out:
    spin_unlock_irqrestore(&lock, was);
    return ok;
}

uint64_t its_pages(void)
{
    return kept;
}

bool its_is_lpi(uint32_t intid)
{
    return its && intid >= LPI_BASE && intid < LPI_BASE + LPIS;
}

uint64_t its_address(void)
{
    return its_pa;
}

/* An LPI enabled at 'level' on the calling CPU, or disabled with 0. */
void its_set(uint32_t intid, unsigned level)
{
    uint32_t id = intid - LPI_BASE;
    unsigned cpu = this_cpu()->index;

    if (!its_is_lpi(intid) || !lpi[id].mapped)
        return;
    unsigned long was = spin_lock_irqsave(&lock);
    if (level && lpi[id].icid != cpu && cpu_ready[cpu]) {
        command(CMD_MOVI | (uint64_t)lpi[id].dev << 32, lpi[id].event, cpu, 0);
        lpi[id].icid = (uint16_t)cpu;
    }
    prop[id] = (uint8_t)(level ? priority_byte(level) | 1 : priority_byte(4));
    clean(&prop[id], 1);
    command(CMD_INV | (uint64_t)lpi[id].dev << 32, lpi[id].event, 0, 0);
    sync_cmd(lpi[id].icid);
    spin_unlock_irqrestore(&lock, was);
}
