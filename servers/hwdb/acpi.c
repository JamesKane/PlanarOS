/*
 * servers/hwdb/acpi.c -- the static ACPI source (ACPI 6.5 §5.2; the
 * IORT is Arm DEN 0049, the DBG2 and SPCR Microsoft's specifications)
 *
 * What needs no AML (§9.4): a node for the GIC's distributor (with its
 * redistributors) and each ITS from the MADT, each MCFG range's host
 * bridge, each IORT SMMUv3 and named component, the SPCR's and DBG2's
 * serial ports, and the GTDT's timer and watchdogs. The IORT's ID
 * mappings become references: a host bridge or named component to its
 * SMMU, and on to the ITS that takes its MSIs.
 *
 * Static tables carry no _HID, so these nodes take the device tree's
 * compatible for what they describe ("arm,gic-v3-its", "arm,smmu-v3"),
 * except the host bridge, which takes PNP0A08 as acpid's will: one match
 * table then binds both ways. Named components get no key: their _HID is
 * in the namespace, which acpid reads (step 4).
 *
 * Fields are unaligned, so every read is a byte at a time.
 */
#include "hwdb.h"

static uint32_t rd8(const uint8_t *p, uint32_t off) { return p[off]; }
static uint32_t rd16(const uint8_t *p, uint32_t off) { return p[off] | (uint32_t)p[off + 1] << 8; }
static uint32_t rd32(const uint8_t *p, uint32_t off) { return rd16(p, off) | rd16(p, off + 2) << 16; }
static uint64_t rd64(const uint8_t *p, uint32_t off) { return rd32(p, off) | (uint64_t)rd32(p, off + 4) << 32; }

constexpr uint32_t HEADER = 36;

struct tables {
    const struct hw_table *t;
    unsigned n;
};

/* A table by signature, its length checked against what was given and
 * its checksum; nullptr if absent or broken. */
static const uint8_t *table(struct hw_graph *g, const struct tables *ts, const char *sig, uint32_t *len)
{
    for (unsigned i = 0; i < ts->n; i++) {
        const uint8_t *p = ts->t[i].data;
        uint32_t n;
        uint8_t sum = 0;

        if (ts->t[i].len < HEADER || p[0] != sig[0] || p[1] != sig[1] || p[2] != sig[2] ||
            p[3] != sig[3])
            continue;
        n = rd32(p, 4);
        if (n < HEADER || n > ts->t[i].len) {
            g->broken++;
            return nullptr;
        }
        for (uint32_t k = 0; k < n; k++)
            sum += p[k];
        if (sum != 0) {
            g->broken++;
            return nullptr;
        }
        *len = n;
        return p;
    }
    return nullptr;
}

/* "<what>@<address in hex>", as a device tree names a node. */
static const char *at_name(struct hw_graph *g, const char *what, uint64_t addr)
{
    struct hw_line l = { 0 }, hex = { 0 };

    hw_put_hex(&hex, addr);
    hw_put(&l, what);
    hw_put(&l, "@");
    hw_put(&l, hex.text + 2);
    return hw_strdup(g, l.text, l.len);
}

static hw_id table_node(struct hw_graph *g, const char *sig)
{
    for (hw_id i = 1; i < g->count; i++)
        if (g->nodes[i].parent == 0 && hw_streq(g->nodes[i].name, sig))
            return i;
    return hw_node_new(g, 0, sig);
}

static hw_id device(struct hw_graph *g, const char *sig, const char *name, const char *key)
{
    hw_id t = table_node(g, sig), n;

    if (t == HW_NONE || (n = hw_node_new(g, t, name)) == HW_NONE)
        return HW_NONE;
    if (key)
        hw_key_add(g, n, key);
    return n;
}

static void mmio(struct hw_graph *g, hw_id n, uint64_t base, uint64_t size)
{
    struct hw_res *r = n == HW_NONE ? nullptr : hw_res_add(g, n);

    if (r) {
        r->kind = HW_MMIO;
        r->base = base;
        r->size = size;
    }
}

/* A GSIV: the GIC's interrupt number, which is the line. */
static void gsiv(struct hw_graph *g, hw_id n, hw_id gic, uint32_t v)
{
    struct hw_res *r;
    struct hw_ref *f;

    if (n == HW_NONE || v == 0 || !(r = hw_res_add(g, n)))
        return;
    r->kind = HW_IRQ;
    r->ctlr = gic;
    r->base = v;
    r->ncells = 1;
    r->cells[0] = v;
    for (unsigned i = 0; i < g->nodes[n].nrefs; i++)
        if (g->nodes[n].refs[i].kind == HW_REF_IRQ)
            return;
    if ((f = hw_ref_add(g, n, HW_REF_IRQ, "GSIV", true)))
        f->target = f->supplier = gic;
}

static void ref(struct hw_graph *g, hw_id from, hw_id to, enum hw_ref_kind kind, const char *name)
{
    struct hw_ref *r;

    if (from == HW_NONE || to == HW_NONE)
        return;
    for (unsigned i = 0; i < g->nodes[from].nrefs; i++)
        if (g->nodes[from].refs[i].kind == kind && g->nodes[from].refs[i].target == to)
            return;
    if ((r = hw_ref_add(g, from, kind, name, false)))
        r->target = r->supplier = to;
}

/* --- The MADT. --- */

constexpr unsigned MAX_ITS = 16;

struct madt {
    hw_id gic;
    unsigned nits;
    uint32_t its_id[MAX_ITS];
    hw_id its[MAX_ITS];
};

static void read_madt(struct hw_graph *g, const struct tables *ts, struct madt *m)
{
    uint32_t len, n;
    const uint8_t *t = table(g, ts, "APIC", &len);

    m->gic = HW_NONE;
    if (!t || len < 44)
        return;
    for (uint32_t off = 44; off <= len - 2; off += n) {
        uint32_t type = rd8(t, off);

        n = rd8(t, off + 1);
        if (n < 2 || n > len - off) {
            g->broken++;
            return;
        }
        if (type == 0x0c && n >= 24 && m->gic == HW_NONE) {        /* GIC distributor */
            uint64_t base = rd64(t, off + 8);
            uint32_t version = rd8(t, off + 20);

            m->gic = device(g, "MADT", at_name(g, "gicd", base),
                            version >= 3 ? "arm,gic-v3" : "arm,cortex-a15-gic");
            mmio(g, m->gic, base, 0x10000);
        } else if (type == 0x0e && n >= 16) {                      /* redistributors */
            mmio(g, m->gic, rd64(t, off + 4), rd32(t, off + 12));
        } else if (type == 0x0f && n >= 20 && m->nits < MAX_ITS) { /* ITS */
            uint64_t base = rd64(t, off + 8);

            m->its_id[m->nits] = rd32(t, off + 4);
            m->its[m->nits] = device(g, "MADT", at_name(g, "its", base), "arm,gic-v3-its");
            mmio(g, m->its[m->nits], base, 0x20000);
            m->nits++;
        }
    }
}

/* --- The MCFG. --- */

static void read_mcfg(struct hw_graph *g, const struct tables *ts)
{
    uint32_t len;
    const uint8_t *t = table(g, ts, "MCFG", &len);

    if (!t)
        return;
    for (uint32_t off = 44; off + 16 <= len; off += 16) {
        uint64_t base = rd64(t, off);
        uint32_t seg = rd16(t, off + 8), first = rd8(t, off + 10), last = rd8(t, off + 11);
        struct hw_line l = { 0 };
        struct hw_res *r;
        hw_id n;

        if (last < first) {
            g->broken++;
            continue;
        }
        hw_put(&l, "pci@");
        hw_put_dec(&l, seg);
        hw_put(&l, ",");
        hw_put_dec(&l, first);
        n = device(g, "MCFG", hw_strdup(g, l.text, l.len), "PNP0A08");
        if (n == HW_NONE || !(r = hw_res_add(g, n)))
            continue;
        r->kind = HW_ECAM;
        r->base = base + ((uint64_t)first << 20);
        r->size = (uint64_t)(last - first + 1) << 20;
        r->ncells = 3;
        r->cells[0] = seg;
        r->cells[1] = first;
        r->cells[2] = last;
    }
}

/* --- The IORT. --- */

enum : uint32_t { IORT_ITS_GROUP = 0, IORT_NAMED = 1, IORT_ROOT_COMPLEX = 2, IORT_SMMU_V3 = 4 };
constexpr unsigned MAX_IORT = 64;

struct iort {
    const uint8_t *t;
    uint32_t len;
    unsigned n;
    uint32_t off[MAX_IORT];         /* each node's offset, its type, its graph node */
    uint32_t type[MAX_IORT];
    hw_id node[MAX_IORT];
};

static int iort_index(const struct iort *io, uint32_t off)
{
    for (unsigned i = 0; i < io->n; i++)
        if (io->off[i] == off)
            return (int)i;
    return -1;
}

/* The ITS nodes an ITS group lists. */
static void its_refs(struct hw_graph *g, const struct iort *io, unsigned grp, const struct madt *m,
                     hw_id from)
{
    uint32_t off = io->off[grp], count = rd32(io->t, off + 16);

    for (uint32_t k = 0; k < count && off + 20 + 4 * (k + 1) <= io->len; k++) {
        uint32_t id = rd32(io->t, off + 20 + 4 * k);

        for (unsigned i = 0; i < m->nits; i++)
            if (m->its_id[i] == id)
                ref(g, from, m->its[i], HW_REF_MSI, "IORT");
    }
}

/* Follow a node's ID mappings: to an SMMU (its IOMMU), and on through the
 * SMMU's mappings to an ITS group, which takes the node's MSIs. */
static void mappings(struct hw_graph *g, const struct iort *io, unsigned i, const struct madt *m,
                     hw_id from, unsigned depth)
{
    uint32_t off = io->off[i], count = rd32(io->t, off + 8), map = rd32(io->t, off + 12);
    uint32_t nlen = rd16(io->t, off + 1);

    if (depth > 2 || map > nlen)
        return;
    for (uint32_t k = 0; k < count && map + 20 * (k + 1) <= nlen; k++) {
        int to = iort_index(io, rd32(io->t, off + map + 20 * k + 12));

        if (to < 0) {
            g->broken++;
            continue;
        }
        if (io->type[to] == IORT_ITS_GROUP) {
            its_refs(g, io, (unsigned)to, m, from);
        } else if (io->type[to] == IORT_SMMU_V3) {
            ref(g, from, io->node[to], HW_REF_IOMMU, "IORT");
            mappings(g, io, (unsigned)to, m, from, depth + 1);
        }
    }
}

/* An ITS group's first ITS's node, for an MSI map's controller. */
static hw_id group_its(const struct iort *io, unsigned grp, const struct madt *m)
{
    uint32_t off = io->off[grp];

    if (rd32(io->t, off + 16) == 0 || off + 24 > io->len)
        return HW_NONE;
    for (unsigned i = 0; i < m->nits; i++)
        if (m->its_id[i] == rd32(io->t, off + 20))
            return m->its[i];
    return HW_NONE;
}

static void msi_map(struct hw_graph *g, hw_id bridge, uint32_t in, uint32_t n, uint32_t out,
                    hw_id its)
{
    struct hw_res *r = hw_res_add(g, bridge);

    if (!r)
        return;
    r->kind = HW_MSIMAP;
    r->base = in;
    r->size = n;
    r->ncells = 1;
    r->cells[0] = out;
    r->ctlr = its;
}

/* A root complex's requester IDs as the ITS sees them: its ID mappings
 * (each 'count' is the IDs less one), through an SMMU's own when they
 * lead to one. Single mappings are a node's own MSIs, not its devices'. */
static void msi_maps(struct hw_graph *g, const struct iort *io, unsigned rc, const struct madt *m,
                     hw_id bridge)
{
    uint32_t off = io->off[rc], count = rd32(io->t, off + 8), map = rd32(io->t, off + 12);
    uint32_t nlen = rd16(io->t, off + 1);

    for (uint32_t k = 0; k < count && map <= nlen && map + 20 * (k + 1) <= nlen; k++) {
        uint32_t e = off + map + 20 * k, in = rd32(io->t, e), n = rd32(io->t, e + 4) + 1;
        uint32_t out = rd32(io->t, e + 8);
        int to = iort_index(io, rd32(io->t, e + 12));

        if (to < 0 || rd32(io->t, e + 16) & 1)
            continue;
        if (io->type[to] == IORT_ITS_GROUP) {
            msi_map(g, bridge, in, n, out, group_its(io, (unsigned)to, m));
            continue;
        }
        if (io->type[to] != IORT_SMMU_V3)
            continue;
        uint32_t so = io->off[to], sc = rd32(io->t, so + 8), sm = rd32(io->t, so + 12);
        uint32_t slen = rd16(io->t, so + 1);

        for (uint32_t q = 0; q < sc && sm <= slen && sm + 20 * (q + 1) <= slen; q++) {
            uint32_t f = so + sm + 20 * q, sin = rd32(io->t, f), sn = rd32(io->t, f + 4) + 1;
            uint64_t lo = out > sin ? out : sin;
            uint64_t hi = (uint64_t)out + n < (uint64_t)sin + sn ? (uint64_t)out + n : (uint64_t)sin + sn;
            int its = iort_index(io, rd32(io->t, f + 12));

            if (rd32(io->t, f + 16) & 1 || its < 0 || io->type[its] != IORT_ITS_GROUP || lo >= hi)
                continue;
            msi_map(g, bridge, (uint32_t)(in + (lo - out)), (uint32_t)(hi - lo),
                    (uint32_t)(rd32(io->t, f + 8) + (lo - sin)), group_its(io, (unsigned)its, m));
        }
    }
}

static void read_iort(struct hw_graph *g, const struct tables *ts, const struct madt *m)
{
    struct iort io = { 0 };
    uint32_t count, off, n;

    io.t = table(g, ts, "IORT", &io.len);
    if (!io.t || io.len < 48)
        return;
    count = rd32(io.t, 36);
    off = rd32(io.t, 40);
    for (uint32_t k = 0; k < count && io.n < MAX_IORT && off >= 48 && off <= io.len - 16;
         k++, off += n) {
        uint32_t type = rd8(io.t, off);
        hw_id node = HW_NONE;

        n = rd16(io.t, off + 1);
        if (n < 16 || n > io.len - off) {
            g->broken++;
            break;
        }
        if (type == IORT_SMMU_V3 && n >= 60) {
            uint64_t base = rd64(io.t, off + 16);

            node = device(g, "IORT", at_name(g, "smmu", base), "arm,smmu-v3");
            mmio(g, node, base, 0x20000);
            for (unsigned q = 0; q < 4; q++)        /* event, PRI, GERR, sync */
                gsiv(g, node, m->gic, rd32(io.t, off + 44 + 4 * q));
        } else if (type == IORT_NAMED && n > 29) {
            uint32_t s = off + 29, e = s;

            while (e < off + n && io.t[e])
                e++;
            if (e < off + n) {
                node = device(g, "IORT", hw_strdup(g, (const char *)io.t + s, e - s), nullptr);
                if (node != HW_NONE)
                    g->nodes[node].state = HW_S_NAMESPACE;
            }
        }
        io.off[io.n] = off;
        io.type[io.n] = type;
        io.node[io.n++] = node;
    }

    /* The mappings, once every node is known. A root complex's apply to
     * each host bridge of its segment. */
    for (unsigned i = 0; i < io.n; i++) {
        if (io.type[i] == IORT_ROOT_COMPLEX && rd16(io.t, io.off[i] + 1) >= 32) {
            uint32_t seg = rd32(io.t, io.off[i] + 28);

            for (hw_id b = 0; b < g->count; b++)
                if (g->nodes[b].nres && g->nodes[b].res[0].kind == HW_ECAM &&
                    g->nodes[b].res[0].cells[0] == seg) {
                    mappings(g, &io, i, m, b, 0);
                    msi_maps(g, &io, i, m, b);
                }
        } else if (io.node[i] != HW_NONE) {
            mappings(g, &io, i, m, io.node[i], 0);
        }
    }
}

/* --- Serial ports, the SPCR's and the DBG2's. --- */

/* Their interface subtypes, Microsoft's "Serial Port Console Redirection
 * Table" and "Debug Port Table 2". */
static const char *uart_key(uint32_t subtype)
{
    switch (subtype) {
    case 0x03:
    case 0x0d:
    case 0x0e: return "arm,pl011";
    case 0x00:
    case 0x01:
    case 0x12: return "ns16550a";
    default:   return nullptr;
    }
}

/* A port described by a generic address structure; HW_NONE if it isn't
 * memory-mapped or of a kind we know. */
static hw_id uart(struct hw_graph *g, const char *sig, uint32_t subtype, const uint8_t *gas)
{
    const char *key = uart_key(subtype);
    uint64_t base = rd64(gas, 4);
    hw_id n;

    if (!key || rd8(gas, 0) != 0 || base == 0)
        return HW_NONE;
    n = device(g, sig, at_name(g, "serial", base), key);
    mmio(g, n, base, 0x1000);
    return n;
}

static void read_serial(struct hw_graph *g, const struct tables *ts, const struct madt *m)
{
    uint32_t len, off, count, n;
    const uint8_t *t;

    if ((t = table(g, ts, "SPCR", &len)) && len >= 80) {
        hw_id u = uart(g, "SPCR", rd8(t, 36), t + 40);

        if (u != HW_NONE) {
            gsiv(g, u, m->gic, rd32(t, 54));
            g->console = u;
        }
    }
    if (!(t = table(g, ts, "DBG2", &len)) || len < 44)
        return;
    off = rd32(t, 36);
    count = rd32(t, 40);
    for (uint32_t i = 0; i < count && off >= 44 && off <= len - 22; i++, off += n) {
        uint32_t gas;

        n = rd16(t, off + 1);
        if (n < 22 || n > len - off) {
            g->broken++;
            return;
        }
        gas = rd16(t, off + 18);
        if (rd16(t, off + 12) == 0x8000 && rd8(t, off + 3) >= 1 && gas <= n - 12)
            uart(g, "DBG2", rd16(t, off + 14), t + off + gas);
    }
}

/* --- The GTDT. --- */

static void read_gtdt(struct hw_graph *g, const struct tables *ts, const struct madt *m)
{
    uint32_t len, count, off, n;
    const uint8_t *t = table(g, ts, "GTDT", &len);
    hw_id timer;

    if (!t || len < 96)
        return;
    timer = device(g, "GTDT", "timer", "arm,armv8-timer");
    for (unsigned q = 0; q < 4; q++)        /* secure EL1, EL1 physical, EL1 virtual, EL2 */
        gsiv(g, timer, m->gic, rd32(t, 48 + 8 * q));
    count = rd32(t, 88);
    off = rd32(t, 92);
    for (uint32_t i = 0; i < count && off >= 96 && off <= len - 4; i++, off += n) {
        hw_id w;

        n = rd16(t, off + 1);
        if (n < 4 || n > len - off) {
            g->broken++;
            return;
        }
        if (rd8(t, off) != 1 || n < 28)
            continue;
        w = device(g, "GTDT", at_name(g, "watchdog", rd64(t, off + 12)), "arm,sbsa-gwdt");
        mmio(g, w, rd64(t, off + 12), 0x1000);      /* control frame */
        mmio(g, w, rd64(t, off + 4), 0x1000);       /* refresh frame */
        gsiv(g, w, m->gic, rd32(t, off + 20));
    }
}

bool hw_from_acpi(struct hw_graph *g, const struct hw_table *t, unsigned n)
{
    struct tables ts = { t, n };
    struct madt m = { 0 };
    const uint8_t *x;
    uint32_t len;
    hw_id root = hw_node_new(g, HW_NONE, "");

    if (root == HW_NONE)
        return false;
    /* The board, for quirks: the XSDT's OEM ID and OEM table ID. */
    if ((x = table(g, &ts, "XSDT", &len))) {
        char id[15];
        unsigned k = 0;

        for (unsigned i = 0; i < 6 && x[10 + i] != ' ' && x[10 + i]; i++)
            id[k++] = (char)x[10 + i];
        id[k++] = ' ';
        for (unsigned i = 0; i < 8 && x[16 + i] != ' ' && x[16 + i]; i++)
            id[k++] = (char)x[16 + i];
        g->board = hw_strdup(g, id, k);
    }
    read_madt(g, &ts, &m);
    read_mcfg(g, &ts);
    read_iort(g, &ts, &m);
    read_serial(g, &ts, &m);
    read_gtdt(g, &ts, &m);
    return g->count > 1 && !g->full;
}
