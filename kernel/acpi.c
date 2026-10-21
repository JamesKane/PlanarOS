/*
 * kernel/acpi.c -- the static ACPI tables
 *
 * The kernel reads them before arch_memory_init() makes RAM normal
 * memory, so through a Device mapping on AArch64, where an unaligned
 * load faults; and ACPI's fields are unaligned (the XSDT's 64-bit
 * entries start at offset 36). So every read here is byte by byte,
 * through rd8() and its wider forms, never a cast.
 *
 * Tables reached by physical address must lie inside the direct map
 * (ARCH_DIRECT_MAP_GIB) as well as pass their checks.
 */
#include "acpi.h"
#include "arch.h"
#include "kprintf.h"
#include "kstring.h"

constexpr unsigned MAX_TABLES = 32;
constexpr unsigned ACPI_REPORT_CLUSTERS = 16;
constexpr uint32_t HEADER = 36;              /* every table's header */
constexpr uint32_t MAX_LENGTH = 16u << 20;   /* larger than any static table */

/* --- Reads, a byte at a time. --- */

static uint8_t rd8(const void *p, uint32_t off)
{
    return ((const volatile uint8_t *)p)[off];
}

static uint16_t rd16(const void *p, uint32_t off)
{
    return (uint16_t)(rd8(p, off) | rd8(p, off + 1) << 8);
}

static uint32_t rd32(const void *p, uint32_t off)
{
    return rd16(p, off) | (uint32_t)rd16(p, off + 2) << 16;
}

static uint64_t rd64(const void *p, uint32_t off)
{
    return rd32(p, off) | (uint64_t)rd32(p, off + 4) << 32;
}

static bool sig_is(const void *p, const char *sig)
{
    for (unsigned i = 0; i < 4; i++)
        if (rd8(p, i) != (uint8_t)sig[i])
            return false;
    return true;
}

static bool sum_ok(const void *p, uint32_t len)
{
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; i++)
        sum += rd8(p, i);
    return sum == 0;
}

/* --- The table index. --- */

struct table {
    const void *p;
    uint32_t len;
};

static struct table tables[MAX_TABLES];
static unsigned ntables, nrefused, nlisted;
static const void *xsdt, *rsdp_at;
static uint32_t xsdt_len;
static uint8_t rsdp_revision;
static char oem_id[7], oem_table_id[9];

/* The table at physical 'pa', mapped, if it's whole and its checksum is
 * good; else nullptr. */
static const void *map_table(uint64_t pa, uint32_t *len)
{
    uint64_t top = (uint64_t)ARCH_DIRECT_MAP_GIB << 30;
    const void *p;
    uint32_t n;

    if (pa == 0 || pa >= top || top - pa < HEADER)
        return nullptr;
    p = phys_to_virt(pa);
    n = rd32(p, 4);
    if (n < HEADER || n > MAX_LENGTH || top - pa < n || !sum_ok(p, n))
        return nullptr;
    *len = n;
    return p;
}

static const struct quirk *quirk;

static void find_quirk(void);

bool acpi_init(const void *rsdp)
{
    uint32_t len, xlen;

    xsdt = rsdp_at = nullptr;
    ntables = nrefused = nlisted = 0;
    quirk = nullptr;
    oem_id[0] = oem_table_id[0] = '\0';
    if (!rsdp)
        return false;

    /* RSDP: ACPI 2.0 or later, so with an XSDT; both checksums good. */
    for (unsigned i = 0; i < 8; i++)
        if (rd8(rsdp, i) != (uint8_t)"RSD PTR "[i])
            return false;
    rsdp_revision = rd8(rsdp, 15);
    len = rd32(rsdp, 20);
    if (rsdp_revision < 2 || len < 36 || len > 4096 || !sum_ok(rsdp, 20) || !sum_ok(rsdp, len))
        return false;
    xsdt = map_table(rd64(rsdp, 24), &xlen);
    if (!xsdt || !sig_is(xsdt, "XSDT")) {
        xsdt = nullptr;
        return false;
    }
    rsdp_at = rsdp;
    xsdt_len = xlen;
    for (unsigned i = 0; i < 6; i++)
        oem_id[i] = (char)rd8(xsdt, 10 + i);
    for (unsigned i = 0; i < 8; i++)
        oem_table_id[i] = (char)rd8(xsdt, 16 + i);
    oem_id[6] = oem_table_id[8] = '\0';

    /* Its entries: whole 8-byte addresses only. */
    for (uint32_t off = HEADER; off + 8 <= xlen; off += 8) {
        const void *t = map_table(rd64(xsdt, off), &len);

        nlisted++;
        if (!t || ntables == MAX_TABLES) {
            nrefused++;
            continue;
        }
        tables[ntables++] = (struct table){ t, len };
    }
    find_quirk();
    return true;
}

const void *acpi_table(const char *sig, uint32_t *len)
{
    for (unsigned i = 0; i < ntables; i++) {
        if (sig_is(tables[i].p, sig)) {
            if (len)
                *len = tables[i].len;
            return tables[i].p;
        }
    }
    return nullptr;
}

bool acpi_blob(unsigned index, uint64_t *pa, uint64_t *len)
{
    uint32_t flen, n;
    const uint8_t *fadt;

    if (!xsdt)
        return false;
    if (index == 0) {
        *pa = virt_to_phys(rsdp_at);
        *len = rd32(rsdp_at, 20);
        return true;
    }
    if (index == 1) {
        *pa = virt_to_phys(xsdt);
        *len = xsdt_len;
        return true;
    }
    if (index - 2 < ntables) {
        *pa = virt_to_phys(tables[index - 2].p);
        *len = tables[index - 2].len;
        return true;
    }
    /* The FADT's X_DSDT (140) and X_FIRMWARE_CTRL (132), or their
     * 32-bit forms (40, 36). */
    index -= 2 + ntables;
    if (index > 1 || !(fadt = acpi_table("FACP", &flen)))
        return false;
    if (index == 0) {
        uint64_t dsdt = flen >= 148 ? rd64(fadt, 140) : 0;
        const void *t = map_table(dsdt ? dsdt : rd32(fadt, 40), &n);

        if (!t)
            return false;
        *pa = virt_to_phys(t);
        *len = n;
        return true;
    }
    uint64_t facs = flen >= 140 ? rd64(fadt, 132) : 0;

    *pa = facs ? facs : rd32(fadt, 36);
    *len = 64;                  /* its fixed part; it has no checksum */
    return *pa != 0;
}

/* --- Board quirks: data, keyed by the XSDT's OEM ID and OEM table ID
 * (docs/09 §9.4). Only what early boot needs is here. --- */

constexpr unsigned QUIRK_DBG2_CONSOLE = 1u << 0;  /* no SPCR: DBG2's port is the console */

struct quirk {
    const char *oem_id, *oem_table_id, *name;
    unsigned flags;
    struct { uint64_t base, size; } reserved[4];
};

static const struct quirk quirks[] = {
    {
        /* CIX P1 (Sky1), the Orange Pi 6 Plus: UART2 is named only by
         * the DBG2, and the DSDT's RSVL carve-outs (the audio DMA, HDA
         * and DSP buffers) are in the EFI map as free memory. */
        .oem_id = "CIXTEK", .oem_table_id = "SKY1EDK2", .name = "CIX P1",
        .flags = QUIRK_DBG2_CONSOLE,
        .reserved = {
            { 0xd0000000, 0x700000 },
            { 0xd0700000, 0x700000 },
            { 0xcde08000, 0x100000 },
        },
    },
};

static void find_quirk(void)
{
    for (size_t i = 0; i < sizeof quirks / sizeof quirks[0]; i++)
        if (kstreq(quirks[i].oem_id, oem_id) && kstreq(quirks[i].oem_table_id, oem_table_id))
            quirk = &quirks[i];
}

const char *acpi_quirk_name(void)
{
    return quirk ? quirk->name : nullptr;
}

bool acpi_reserved(unsigned index, uint64_t *base, uint64_t *size)
{
    if (!quirk || index >= sizeof quirk->reserved / sizeof quirk->reserved[0] ||
        !quirk->reserved[index].size)
        return false;
    *base = quirk->reserved[index].base;
    *size = quirk->reserved[index].size;
    return true;
}

/* --- The MADT. --- */

constexpr uint32_t MADT_ENTRIES = 44;
constexpr uint8_t MADT_GICC = 0x0b, MADT_GICD = 0x0c, MADT_GICR = 0x0e, MADT_ITS = 0x0f;
constexpr uint32_t GICC_LEN = 76;           /* ACPI 5.1's; 6.0 adds the efficiency class */
constexpr uint32_t GICC_ENABLED = 1u << 0;

/*
 * The MADT's next entry of type 'type' after offset *off (0: from the
 * first), whose length is at least 'min': its offset, or 0 when there
 * are no more. An entry shorter than its header, or running past the
 * table, ends the walk; one too short for its type is skipped.
 */
static uint32_t madt_next(uint32_t *off, uint8_t type, uint32_t min)
{
    uint32_t len, at;
    const void *m = acpi_table("APIC", &len);

    if (!m)
        return 0;
    at = *off ? *off + rd8(m, *off + 1) : MADT_ENTRIES;
    for (; at + 2 <= len; at += rd8(m, at + 1)) {
        uint32_t n = rd8(m, at + 1);

        if (n < 2 || at + n > len)
            return 0;
        if (rd8(m, at) == type && n >= min)
            return *off = at;
    }
    return 0;
}

bool acpi_cpu(unsigned index, struct acpi_cpu *c)
{
    const void *m = acpi_table("APIC", nullptr);
    uint32_t off = 0;

    for (unsigned i = 0; madt_next(&off, MADT_GICC, GICC_LEN); i++) {
        if (i < index)
            continue;
        *c = (struct acpi_cpu){
            .mpidr = rd64(m, off + 68),
            .uid = rd32(m, off + 8),
            .enabled = rd32(m, off + 12) & GICC_ENABLED,
            .gicr = rd64(m, off + 60),
            .efficiency = rd8(m, off + 1) > 76 ? rd8(m, off + 76) : 0,
        };
        return true;
    }
    return false;
}

bool acpi_gic(struct acpi_gic *g)
{
    const void *m = acpi_table("APIC", nullptr);
    struct acpi_cpu c;
    uint32_t off = 0;

    *g = (struct acpi_gic){};
    if (!madt_next(&off, MADT_GICD, 24))
        return false;
    g->gicd = rd64(m, off + 8);
    g->version = rd8(m, off + 20);
    for (off = 0; madt_next(&off, MADT_GICR, 16) && g->ngicr < ACPI_MAX_GICR; )
        g->gicr[g->ngicr++] = (struct acpi_range){ rd64(m, off + 4), rd32(m, off + 12) };
    /* No GICR ranges: each enabled CPU's GICC entry names its own. */
    if (!g->ngicr)
        for (unsigned i = 0; acpi_cpu(i, &c) && g->ngicr < ACPI_MAX_GICR; i++)
            if (c.enabled && c.gicr)
                g->gicr[g->ngicr++] = (struct acpi_range){ c.gicr, 0 };
    for (off = 0; madt_next(&off, MADT_ITS, 20) && g->nits < ACPI_MAX_ITS; )
        g->its[g->nits++] = rd64(m, off + 8);
    return g->gicd && g->ngicr;
}

/* --- The GTDT. --- */

constexpr uint32_t GTDT_LEN = 96;           /* through the platform timers' offset */
constexpr uint8_t GTDT_WATCHDOG = 1;

bool acpi_timer(struct acpi_timer *t)
{
    uint32_t len;
    const void *g = acpi_table("GTDT", &len);

    if (!g || len < GTDT_LEN)
        return false;
    *t = (struct acpi_timer){
        .el1_phys = rd32(g, 56), .el1_virt = rd32(g, 64), .el2_phys = rd32(g, 72),
    };
    return true;
}

bool acpi_watchdog(unsigned index, struct acpi_watchdog *w)
{
    uint32_t len, count, off, n;
    const void *g = acpi_table("GTDT", &len);

    if (!g || len < GTDT_LEN)
        return false;
    count = rd32(g, 88);
    off = rd32(g, 92);
    /* Platform timers have 16-bit lengths; the same rules as the MADT's. */
    for (uint32_t i = 0; i < count && off >= GTDT_LEN && off <= len - 4; i++, off += n) {
        n = rd16(g, off + 1);
        if (n < 4 || n > len - off)
            return false;
        if (rd8(g, off) != GTDT_WATCHDOG || n < 28)
            continue;
        if (index-- == 0) {
            *w = (struct acpi_watchdog){
                .refresh = rd64(g, off + 4), .control = rd64(g, off + 12),
                .gsiv = rd32(g, off + 20),
            };
            return true;
        }
    }
    return false;
}

/* --- The console: SPCR, then DBG2. --- */

/* SPCR and DBG2 interface subtypes (Microsoft's "Serial Port Console
 * Redirection Table" and "Debug Port Table 2"). */
constexpr uint16_t UART_16550 = 0x00, UART_16550_SUBSET = 0x01, UART_PL011 = 0x03,
                   UART_SBSA_32 = 0x0d, UART_SBSA = 0x0e, UART_16550_GAS = 0x12;
constexpr uint16_t DBG2_SERIAL = 0x8000;

/* An interface subtype and the generic address of its registers (a
 * GAS: space, width, offset, access size, address) as a console. */
static bool console_from(uint16_t subtype, const void *gas, const char *table,
                         struct acpi_console *c)
{
    uint8_t access = rd8(gas, 3);

    if (rd8(gas, 0) != 0 || !rd64(gas, 4))
        return false;                       /* not memory-mapped */
    *c = (struct acpi_console){ .base = rd64(gas, 4), .table = table };
    switch (subtype) {
    case UART_PL011:
    case UART_SBSA_32:
    case UART_SBSA:
        c->kind = ACPI_UART_PL011;
        return true;
    case UART_16550:
    case UART_16550_SUBSET:
    case UART_16550_GAS:
        /* Access size 1-4 is 8 to 64 bits, registers that far apart;
         * 0 (legacy) means bytes. */
        c->kind = ACPI_UART_16550;
        c->reg_shift = access >= 1 && access <= 3 ? access - 1 : 0;
        c->reg_width = (uint8_t)(1u << c->reg_shift);
        return true;
    default:
        return false;
    }
}

static bool dbg2_console(struct acpi_console *c)
{
    uint32_t len, off, count, n;
    const void *d = acpi_table("DBG2", &len);

    if (!d || len < 44)
        return false;
    off = rd32(d, 36);
    count = rd32(d, 40);
    for (uint32_t i = 0; i < count && off >= 44 && off <= len - 22; i++, off += n) {
        uint32_t gas;

        n = rd16(d, off + 1);
        if (n < 22 || n > len - off)
            return false;
        gas = rd16(d, off + 18);
        if (rd16(d, off + 12) != DBG2_SERIAL || rd8(d, off + 3) < 1 || gas > n - 12)
            continue;
        if (console_from(rd16(d, off + 14), (const uint8_t *)d + off + gas, "DBG2", c))
            return true;
    }
    return false;
}

bool acpi_console(struct acpi_console *c)
{
    uint32_t len;
    const void *s = acpi_table("SPCR", &len);

    if (s && len >= 80 && console_from(rd8(s, 36), (const uint8_t *)s + 40, "SPCR", c))
        return true;
    return quirk && (quirk->flags & QUIRK_DBG2_CONSOLE) && dbg2_console(c);
}

/* --- The FADT and the MCFG. --- */

constexpr uint16_t ARM_PSCI_COMPLIANT = 1u << 0, ARM_PSCI_USE_HVC = 1u << 1;

bool acpi_psci(bool *smc)
{
    uint32_t len;
    const void *f = acpi_table("FACP", &len);
    uint16_t flags;

    if (!f || len < 131)
        return false;
    flags = rd16(f, 129);
    *smc = !(flags & ARM_PSCI_USE_HVC);
    return flags & ARM_PSCI_COMPLIANT;
}

bool acpi_pci(unsigned index, struct acpi_pci *p)
{
    uint32_t len;
    const void *m = acpi_table("MCFG", &len);
    uint32_t off = 44 + 16 * index;

    if (!m || len < 44 || index >= (len - 44) / 16)
        return false;
    *p = (struct acpi_pci){
        .base = rd64(m, off), .segment = rd16(m, off + 8),
        .bus_start = rd8(m, off + 10), .bus_end = rd8(m, off + 11),
    };
    return true;
}

/* --- The PPTT: which cluster each CPU is in. --- */

constexpr uint32_t PPTT_ENTRIES = 36;
constexpr uint8_t PPTT_PROCESSOR = 0;
constexpr uint32_t PPTT_PROC_LEN = 20;
constexpr uint32_t PPTT_ID_VALID = 1u << 1, PPTT_THREAD = 1u << 2, PPTT_LEAF = 1u << 3;

/* Whether a processor node starts at 'off', whole, inside the table. */
static bool pptt_node(const void *t, uint32_t len, uint32_t off)
{
    return off >= PPTT_ENTRIES && off <= len - PPTT_PROC_LEN && rd8(t, off) == PPTT_PROCESSOR &&
           rd8(t, off + 1) >= PPTT_PROC_LEN && rd8(t, off + 1) <= len - off;
}

/* Whether any processor node names 'off' as its parent. */
static bool pptt_has_child(const void *t, uint32_t len, uint32_t off)
{
    for (uint32_t at = PPTT_ENTRIES, n; at + 2 <= len; at += n) {
        n = rd8(t, at + 1);
        if (n < 2 || n > len - at)
            return false;
        if (pptt_node(t, len, at) && rd32(t, at + 8) == off)
            return true;
    }
    return false;
}

/*
 * The leaf for ACPI processor 'uid', and its parent: a core's parent is
 * its cluster, a thread's is a core, whose parent is the cluster. A
 * leaf is flagged as one (ACPI 6.3) or is nobody's parent; a package or
 * cluster may carry an ID of its own that matches a CPU's UID, as the
 * CIX's package does. Every parent must be a whole processor node, or
 * the CPU has no cluster.
 */
bool acpi_cluster(uint32_t uid, uint32_t *cluster)
{
    uint32_t len, flags, up;
    const void *t = acpi_table("PPTT", &len);

    if (!t)
        return false;
    for (uint32_t at = PPTT_ENTRIES, n; at + 2 <= len; at += n) {
        n = rd8(t, at + 1);
        if (n < 2 || n > len - at)
            return false;
        if (!pptt_node(t, len, at))
            continue;
        flags = rd32(t, at + 4);
        if (!(flags & PPTT_ID_VALID) || rd32(t, at + 12) != uid ||
            (!(flags & PPTT_LEAF) && pptt_has_child(t, len, at)))
            continue;
        up = rd32(t, at + 8);
        if (flags & PPTT_THREAD)
            up = pptt_node(t, len, up) && up != at ? rd32(t, up + 8) : 0;
        if (!pptt_node(t, len, up) || up == at)
            return false;
        *cluster = up;
        return true;
    }
    return false;
}

/* --- The report. --- */

/* Every table, as acpi_blob gives them, in hex ("acpi: " lines, each
 * table ending "acpi: end SIG, N bytes"), for tools/acpi-from-log.py:
 * the boot argument "dumpacpi" (docs/boards/qemu-virt/README.md). */
void acpi_dump_tables(void)
{
    static const char hex[] = "0123456789abcdef";
    char line[64 * 2 + 1];
    uint64_t pa, len;

    for (unsigned t = 1; acpi_blob(t, &pa, &len); t++) {
        const uint8_t *b = phys_to_virt(pa);

        for (uint64_t off = 0; off < len; off += 64) {
            uint64_t n = len - off < 64 ? len - off : 64;

            for (uint64_t i = 0; i < n; i++) {
                line[2 * i] = hex[b[off + i] >> 4];
                line[2 * i + 1] = hex[b[off + i] & 15];
            }
            line[2 * n] = 0;
            kprintf("acpi: %s\n", line);
        }
        kprintf("acpi: end %c%c%c%c, %llu bytes\n", b[0], b[1], b[2], b[3],
                (unsigned long long)len);
    }
}

void acpi_report(void)
{
    struct acpi_gic g;
    struct acpi_cpu c;
    struct acpi_timer t;
    struct acpi_watchdog w;
    struct acpi_console con;
    struct acpi_pci pci;
    unsigned ncpu = 0, enabled = 0;
    bool smc;
    char sigs[MAX_TABLES * 5 + 1];
    size_t n = 0;

    for (unsigned i = 0; i < ntables; i++) {
        for (unsigned j = 0; j < 4; j++)
            sigs[n++] = (char)rd8(tables[i].p, j);
        sigs[n++] = ' ';
    }
    sigs[n ? n - 1 : 0] = '\0';
    kprintf("planar: ACPI from %s %s (RSDP revision %u): %s", oem_id, oem_table_id,
            (unsigned)rsdp_revision, sigs);
    if (nrefused)
        kprintf("; %u of %u refused", nrefused, nlisted);
    kprintf("\n");
    if (quirk) {
        uint64_t base, size;

        kprintf("planar: ACPI quirks for the %s:%s", quirk->name,
                quirk->flags & QUIRK_DBG2_CONSOLE ? " the console from the DBG2;" : "");
        kprintf(" reserved");
        for (unsigned i = 0; acpi_reserved(i, &base, &size); i++)
            kprintf(" %p+%llx", (void *)base, (unsigned long long)size);
        kprintf("\n");
    }

    for (unsigned i = 0; acpi_cpu(i, &c); i++) {
        ncpu++;
        enabled += c.enabled;
    }
    if (acpi_gic(&g)) {
        kprintf("planar: ACPI GICv%u: GICD %p, GICR", g.version, (void *)g.gicd);
        for (unsigned i = 0; i < g.ngicr; i++)
            kprintf(" %p+%llx", (void *)g.gicr[i].base, (unsigned long long)g.gicr[i].size);
        for (unsigned i = 0; i < g.nits; i++)
            kprintf("%s%p", i ? " " : ", ITS ", (void *)g.its[i]);
        kprintf("\n");
    }
    kprintf("planar: ACPI %u CPUs, %u enabled; UID:MPIDR", ncpu, enabled);
    for (unsigned i = 0; acpi_cpu(i, &c); i++)
        kprintf(" %u:%llx%s", c.uid, (unsigned long long)c.mpidr, c.enabled ? "" : "(off)");
    kprintf("\n");
    if (acpi_table("PPTT", nullptr)) {
        /* Each cluster's UIDs, in the MADT's order of its first CPU. */
        uint32_t seen[ACPI_REPORT_CLUSTERS], cl, cl2;
        unsigned nseen = 0;

        kprintf("planar: ACPI PPTT clusters, by UID:");
        for (unsigned i = 0; acpi_cpu(i, &c); i++) {
            bool dup = false;

            if (!acpi_cluster(c.uid, &cl)) {
                kprintf(" %u(none)", c.uid);
                continue;
            }
            for (unsigned k = 0; k < nseen; k++)
                dup |= seen[k] == cl;
            if (dup || nseen == ACPI_REPORT_CLUSTERS)
                continue;
            seen[nseen++] = cl;
            kprintf(" ");
            for (unsigned j = 0, first = 1; acpi_cpu(j, &c); j++)
                if (acpi_cluster(c.uid, &cl2) && cl2 == cl) {
                    kprintf("%s%u", first ? "" : ",", c.uid);
                    first = 0;
                }
        }
        kprintf("\n");
    }
    if (acpi_timer(&t))
        kprintf("planar: ACPI timer: EL1 physical %u, virtual %u, EL2 %u\n", t.el1_phys,
                t.el1_virt, t.el2_phys);
    for (unsigned i = 0; acpi_watchdog(i, &w); i++)
        kprintf("planar: ACPI watchdog: refresh %p, control %p, GSIV %u\n",
                (void *)w.refresh, (void *)w.control, w.gsiv);
    if (acpi_console(&con))
        kprintf("planar: ACPI console from the %s: %s at %p\n", con.table,
                con.kind == ACPI_UART_PL011 ? "PL011" : "16550", (void *)con.base);
    if (acpi_psci(&smc))
        kprintf("planar: ACPI PSCI over %s\n", smc ? "SMC" : "HVC");
    for (unsigned i = 0; acpi_pci(i, &pci); i++)
        kprintf("planar: ACPI PCI segment %u, buses %u-%u at %p\n", pci.segment,
                pci.bus_start, pci.bus_end, (void *)pci.base);
}
