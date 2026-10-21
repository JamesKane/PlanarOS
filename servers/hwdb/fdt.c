/*
 * servers/hwdb/fdt.c -- the device tree source (Devicetree Specification
 * v0.4, §5 for the blob, §2.3-2.4 for the properties read here)
 *
 * One pass over the structure block makes a node for each node in the
 * tree, with its keys (compatible), whether it's enabled, and its
 * phandle. A second pass, once every phandle is known, gives each device
 * its registers (reg, through each bus's ranges), its interrupts (through
 * interrupt-parent, interrupts-extended and interrupt-map) and its
 * references to other devices (clocks, resets, power domains, supplies,
 * phys, pins, GPIOs, and the optional DMA, IOMMU, MSI and nvmem ones,
 * with PCI's msi-map and iommu-map).
 *
 * Names, keys and property values are read in place, so the blob must
 * stay where it is while the graph is used.
 */
#include "hwdb.h"

enum : uint32_t { FDT_MAGIC = 0xd00dfeed, BEGIN_NODE = 1, END_NODE = 2, PROP = 3, NOP = 4, END = 9 };

constexpr unsigned MAX_DEPTH = 32;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t tok(const struct hw_graph *g, uint32_t off)
{
    return off <= g->fdt_struct_size - 4 ? be32(g->fdt + g->fdt_struct + off) : END;
}

/* A NUL-terminated string at 'off' in the structure block, or nullptr. */
static const char *struct_str(const struct hw_graph *g, uint32_t off, uint32_t *len)
{
    const char *s = (const char *)g->fdt + g->fdt_struct;

    for (uint32_t i = off; i < g->fdt_struct_size; i++)
        if (s[i] == 0) {
            *len = i - off;
            return s + off;
        }
    return nullptr;
}

static const char *prop_name(const struct hw_graph *g, uint32_t nameoff)
{
    const char *s = (const char *)g->fdt + g->fdt_strings;

    if (nameoff >= g->fdt_strings_size)
        return nullptr;
    for (uint32_t i = nameoff; i < g->fdt_strings_size; i++)
        if (s[i] == 0)
            return s + nameoff;
    return nullptr;
}

/* The property at 'off' (a PROP token): its name, value and the offset of
 * the token after it; false if it runs off the block. */
static bool prop_at(const struct hw_graph *g, uint32_t off, const char **name,
                    const uint8_t **value, uint32_t *len, uint32_t *next)
{
    uint32_t n;

    if (off > g->fdt_struct_size - 12)
        return false;
    n = tok(g, off + 4);
    if (n > g->fdt_struct_size - off - 12)
        return false;
    *name = prop_name(g, tok(g, off + 8));
    *value = g->fdt + g->fdt_struct + off + 12;
    *len = n;
    *next = off + 12 + ((n + 3) & ~3u);
    return *name != nullptr;
}

const void *hw_fdt_prop(const struct hw_graph *g, hw_id node, const char *name, uint32_t *len)
{
    uint32_t off, next;
    const char *pn;
    const uint8_t *value;

    if (!g->fdt || node == HW_NONE)
        return nullptr;
    for (off = g->nodes[node].fdt_props;; off = next) {
        uint32_t t = tok(g, off);

        if (t == NOP) {
            next = off + 4;
            continue;
        }
        if (t != PROP || !prop_at(g, off, &pn, &value, len, &next))
            return nullptr;
        if (hw_streq(pn, name))
            return value;
    }
}

static uint32_t prop_u32(const struct hw_graph *g, hw_id node, const char *name, uint32_t fallback)
{
    uint32_t len;
    const uint8_t *p = hw_fdt_prop(g, node, name, &len);

    return p && len == 4 ? be32(p) : fallback;
}

static hw_id by_phandle(const struct hw_graph *g, uint32_t ph)
{
    uint32_t lo = 0, hi = g->nphandles;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;

        if (g->phandles[2 * mid] == ph)
            return g->phandles[2 * mid + 1];
        if (g->phandles[2 * mid] < ph)
            lo = mid + 1;
        else
            hi = mid;
    }
    return HW_NONE;
}

/* --- Pass 1: the nodes. --- */

static void read_node_props(struct hw_graph *g, hw_id id, uint32_t off)
{
    struct hw_node *n = &g->nodes[id];
    uint32_t len, next;
    const char *name;
    const uint8_t *v;

    n->fdt_props = off;
    for (;; off = next) {
        uint32_t t = tok(g, off);

        if (t == NOP) {
            next = off + 4;
            continue;
        }
        if (t != PROP || !prop_at(g, off, &name, &v, &len, &next))
            return;
        if (hw_streq(name, "compatible") && len > 0 && v[len - 1] == 0) {
            for (uint32_t i = 0; i < len; i += (uint32_t)hw_strlen((const char *)v + i) + 1)
                if (v[i] && !hw_key_add(g, id, (const char *)v + i))
                    return;
        } else if (hw_streq(name, "status") && len > 0 && v[len - 1] == 0) {
            const char *s = (const char *)v;

            n->enabled = hw_streq(s, "okay") || hw_streq(s, "ok");
        } else if ((hw_streq(name, "phandle") || hw_streq(name, "linux,phandle")) && len == 4) {
            n->phandle = be32(v);
        }
    }
}

static bool build_nodes(struct hw_graph *g)
{
    hw_id stack[MAX_DEPTH];
    unsigned depth = 0;
    uint32_t off = 0, len, next;

    for (;;) {
        uint32_t t = tok(g, off);
        const char *name;
        const uint8_t *v;

        switch (t) {
        case BEGIN_NODE: {
            const char *nm = struct_str(g, off + 4, &len);
            hw_id id;

            if (!nm || depth == MAX_DEPTH)
                return false;
            id = hw_node_new(g, depth ? stack[depth - 1] : HW_NONE, nm);
            if (id == HW_NONE)
                return false;
            off += 4 + ((len + 1 + 3) & ~3u);
            read_node_props(g, id, off);
            if (depth && !g->nodes[stack[depth - 1]].enabled)
                g->nodes[id].enabled = false;
            stack[depth++] = id;
            break;
        }
        case PROP:
            if (!prop_at(g, off, &name, &v, &len, &next))
                return false;
            off = next;
            break;
        case NOP:
            off += 4;
            break;
        case END_NODE:
            if (depth == 0)
                return false;
            depth--;
            off += 4;
            break;
        case END:
            return depth == 0 && g->count > 0;
        default:
            return false;
        }
    }
}

static bool index_phandles(struct hw_graph *g)
{
    uint32_t n = 0;

    for (hw_id i = 0; i < g->count; i++)
        n += g->nodes[i].phandle != 0;
    g->phandles = hw_alloc(g->arena, 2 * (n ? n : 1) * sizeof *g->phandles);
    if (!g->phandles)
        return false;
    /* Insertion sort: trees have hundreds of phandles, mostly in order. */
    for (hw_id i = 0; i < g->count; i++) {
        uint32_t ph = g->nodes[i].phandle, j = g->nphandles;

        if (ph == 0)
            continue;
        for (; j > 0 && g->phandles[2 * (j - 1)] > ph; j--) {
            g->phandles[2 * j] = g->phandles[2 * (j - 1)];
            g->phandles[2 * j + 1] = g->phandles[2 * (j - 1) + 1];
        }
        if (j > 0 && g->phandles[2 * (j - 1)] == ph) {
            g->broken++;                    /* a duplicate: the first one wins */
            for (; j < g->nphandles; j++) {
                g->phandles[2 * j] = g->phandles[2 * (j + 1)];
                g->phandles[2 * j + 1] = g->phandles[2 * (j + 1) + 1];
            }
            continue;
        }
        g->phandles[2 * j] = ph;
        g->phandles[2 * j + 1] = i;
        g->nphandles++;
    }
    return true;
}

/* --- Pass 2: registers. --- */

static uint64_t cells_value(const uint8_t *p, uint32_t cells)
{
    uint64_t v = 0;

    for (uint32_t i = 0; i < cells; i++)
        v = v << 32 | be32(p + 4 * i);
    return v;
}

/*
 * Translate 'addr', on the bus 'bus' (the node's parent), up to the root
 * through each bus's ranges. False if some bus on the way has no ranges
 * (I2C, SPI, /cpus: its addresses aren't memory) or none covers it.
 */
static bool translate(const struct hw_graph *g, hw_id bus, uint64_t *addr)
{
    for (; bus != HW_NONE && g->nodes[bus].parent != HW_NONE; bus = g->nodes[bus].parent) {
        hw_id up = g->nodes[bus].parent;
        uint32_t len, cac = prop_u32(g, bus, "#address-cells", 2);
        uint32_t csc = prop_u32(g, bus, "#size-cells", 1), pac = prop_u32(g, up, "#address-cells", 2);
        const uint8_t *r = hw_fdt_prop(g, bus, "ranges", &len);
        uint32_t entry = 4 * (cac + pac + csc);
        bool found = false;

        if (!r)
            return false;
        if (len == 0)
            continue;
        if (cac > 2 || pac > 2 || csc > 2 || entry == 0)
            return false;
        for (uint32_t o = 0; o + entry <= len; o += entry) {
            uint64_t cbase = cells_value(r + o, cac), pbase = cells_value(r + o + 4 * cac, pac);
            uint64_t size = cells_value(r + o + 4 * (cac + pac), csc);

            if (*addr >= cbase && *addr - cbase < size) {
                *addr = pbase + (*addr - cbase);
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static void read_reg(struct hw_graph *g, hw_id id)
{
    hw_id bus = g->nodes[id].parent;
    uint32_t len, ac = prop_u32(g, bus, "#address-cells", 2), sc = prop_u32(g, bus, "#size-cells", 1);
    const uint8_t *p = hw_fdt_prop(g, id, "reg", &len);
    uint32_t entry = 4 * (ac + sc);

    if (!p || entry == 0)
        return;
    if (ac > 2 || sc > 2) {
        g->broken++;                        /* PCI's three-cell addresses, say */
        return;
    }
    for (uint32_t o = 0; o + entry <= len; o += entry) {
        uint64_t addr = cells_value(p + o, ac), size = cells_value(p + o + 4 * ac, sc);
        struct hw_res *r = hw_res_add(g, id);

        if (!r)
            return;
        r->base = addr;
        r->size = size;
        r->kind = translate(g, bus, &r->base) ? HW_MMIO : HW_BUSADDR;
        if (r->kind == HW_BUSADDR)
            r->base = addr;
    }
}

/*
 * A PCI host bridge's (device_type "pci", PCI Bus Binding to IEEE 1275):
 * its reg is ECAM, with bus-range's buses, and each ranges entry is a
 * window, three cells of PCI address (space and flags, then the bus
 * address) to the parent's.
 */
static void read_pci_host(struct hw_graph *g, hw_id id)
{
    struct hw_node *n = &g->nodes[id];
    uint32_t len, pac = prop_u32(g, n->parent, "#address-cells", 2);
    uint32_t sc = prop_u32(g, id, "#size-cells", 2), entry = 4 * (3 + pac + sc);
    const char *type = hw_fdt_prop(g, id, "device_type", &len);
    const uint8_t *p;

    if (!type || len != 4 || !hw_streq(type, "pci") || !hw_has_key(n, "pci-host-ecam-generic") ||
        n->nres == 0 || n->res[0].kind != HW_MMIO)
        return;
    n->res[0].kind = HW_ECAM;
    n->res[0].ncells = 3;
    n->res[0].cells[0] = prop_u32(g, id, "linux,pci-domain", 0);
    n->res[0].cells[1] = 0;
    n->res[0].cells[2] = (uint32_t)((n->res[0].size >> 20) - 1);
    if ((p = hw_fdt_prop(g, id, "bus-range", &len)) && len == 8) {
        n->res[0].cells[1] = be32(p);
        n->res[0].cells[2] = be32(p + 4);
    }
    if (!(p = hw_fdt_prop(g, id, "ranges", &len)) || pac > 2 || sc > 2)
        return;
    for (uint32_t o = 0; o + entry <= len; o += entry) {
        uint32_t space = be32(p + o) >> 24 & 3;     /* 1 I/O, 2 32-bit memory, 3 64-bit */
        uint64_t bus = cells_value(p + o + 4, 2), cpu = cells_value(p + o + 12, pac);
        struct hw_res *r;

        if (space == 0 || !(r = hw_res_add(g, id)))
            continue;
        r->kind = HW_WINDOW;
        r->base = cpu;
        r->size = cells_value(p + o + 12 + 4 * pac, sc);
        r->ncells = 3;
        r->cells[0] = (space >= 2 ? HW_WIN_MEM : 0) | (space == 3 ? HW_WIN_64 : 0) |
                      (be32(p + o) & 1u << 30 ? HW_WIN_PREFETCH : 0);
        r->cells[1] = (uint32_t)(bus >> 32);
        r->cells[2] = (uint32_t)bus;
        if (!translate(g, n->parent, &r->base))
            g->broken++;
    }
}

/* --- Pass 2: interrupts. --- */

/* The line a lease would name, for the controllers the kernel drives. */
static uint64_t irq_line(const struct hw_node *c, const uint32_t *cells, uint32_t n)
{
    if (hw_has_key(c, "arm,gic-v3") || hw_has_key(c, "arm,gic-400") ||
        hw_has_key(c, "arm,cortex-a15-gic")) {
        if (n >= 2 && cells[0] == 0)
            return 32 + (uint64_t)cells[1];         /* SPI */
        if (n >= 2 && cells[0] == 1)
            return 16 + (uint64_t)cells[1];         /* PPI */
        return HW_NOLINE;
    }
    if (hw_has_key(c, "riscv,plic0") || hw_has_key(c, "sifive,plic-1.0.0") ||
        hw_has_key(c, "riscv,cpu-intc"))
        return n >= 1 ? cells[0] : HW_NOLINE;
    return HW_NOLINE;
}

/* One reference per controller, however many lines go to it. */
static void irq_ref(struct hw_graph *g, hw_id id, hw_id ctlr, uint32_t ph, const char *prop)
{
    struct hw_node *n = &g->nodes[id];
    struct hw_ref *r;

    for (unsigned i = 0; i < n->nrefs; i++)
        if (n->refs[i].kind == HW_REF_IRQ && n->refs[i].target == ctlr &&
            (ctlr != HW_NONE || n->refs[i].handle == ph))
            return;
    r = hw_ref_add(g, id, HW_REF_IRQ, prop, true);
    if (r) {
        r->target = ctlr;
        r->handle = ph;
    }
}

static void add_irq(struct hw_graph *g, hw_id id, hw_id ctlr, const uint8_t *p, uint32_t ncells,
                    const char *prop)
{
    struct hw_res *r = hw_res_add(g, id);

    irq_ref(g, id, ctlr, 0, prop);
    if (!r)
        return;
    r->kind = HW_IRQ;
    r->ctlr = ctlr;
    r->ncells = (uint8_t)(ncells < HW_CELLS ? ncells : HW_CELLS);
    for (uint32_t i = 0; i < r->ncells; i++)
        r->cells[i] = be32(p + 4 * i);
    r->base = irq_line(&g->nodes[ctlr], r->cells, r->ncells);
}

static hw_id interrupt_parent(const struct hw_graph *g, hw_id id, uint32_t *ph)
{
    for (hw_id n = id; n != HW_NONE; n = g->nodes[n].parent) {
        uint32_t p = prop_u32(g, n, "interrupt-parent", 0);

        if (p) {
            *ph = p;
            return by_phandle(g, p);
        }
    }
    *ph = 0;
    return HW_NONE;
}

static void read_interrupts(struct hw_graph *g, hw_id id)
{
    uint32_t len, ph, cells;
    const uint8_t *p;
    hw_id ctlr;

    if ((p = hw_fdt_prop(g, id, "interrupts-extended", &len))) {
        for (uint32_t o = 0; o + 4 <= len; o += 4 * (cells + 1)) {
            ph = be32(p + o);
            ctlr = by_phandle(g, ph);
            if (ctlr == HW_NONE) {
                irq_ref(g, id, HW_NONE, ph, "interrupts-extended");
                return;
            }
            cells = prop_u32(g, ctlr, "#interrupt-cells", 1);
            if (o + 4 * (cells + 1) > len)
                return;
            /* A PLIC context no hart takes (M-mode's, under OpenSBI). */
            if (cells == 1 && be32(p + o + 4) == UINT32_MAX)
                continue;
            add_irq(g, id, ctlr, p + o + 4, cells, "interrupts-extended");
        }
    } else if ((p = hw_fdt_prop(g, id, "interrupts", &len))) {
        ctlr = interrupt_parent(g, id, &ph);
        if (ctlr == HW_NONE) {
            irq_ref(g, id, HW_NONE, ph, "interrupts");
            return;
        }
        cells = prop_u32(g, ctlr, "#interrupt-cells", 1);
        if (cells == 0)
            return;
        for (uint32_t o = 0; o + 4 * cells <= len; o += 4 * cells)
            add_irq(g, id, ctlr, p + o, cells, "interrupts");
    }

    /* A nexus (a PCI host bridge): each entry's parent is a supplier. */
    if ((p = hw_fdt_prop(g, id, "interrupt-map", &len))) {
        uint32_t cac = prop_u32(g, id, "#address-cells", 0), cic = prop_u32(g, id, "#interrupt-cells", 1);

        for (uint32_t o = 0; o + 4 * (cac + cic + 1) <= len;) {
            o += 4 * (cac + cic);
            ph = be32(p + o);
            ctlr = by_phandle(g, ph);
            irq_ref(g, id, ctlr, ph, "interrupt-map");
            if (ctlr == HW_NONE)
                return;
            o += 4 * (1 + prop_u32(g, ctlr, "#address-cells", 0) +
                      prop_u32(g, ctlr, "#interrupt-cells", 1));
        }
    }
}

/* --- Pass 2: references. --- */

static bool ends_with(const char *s, const char *tail)
{
    size_t n = hw_strlen(s), t = hw_strlen(tail);

    return n > t && hw_streq(s + n - t, tail);
}

/* A list of phandles, each followed by the cells its target's 'cells'
 * property says (none if 'cells' is nullptr). */
static void phandle_list(struct hw_graph *g, hw_id id, const char *prop, const uint8_t *p,
                         uint32_t len, enum hw_ref_kind kind, const char *cells, bool required)
{
    for (uint32_t o = 0; o + 4 <= len;) {
        uint32_t ph = be32(p + o), n;
        hw_id t = by_phandle(g, ph);
        struct hw_ref *r;

        o += 4;
        if (ph == 0)
            continue;                       /* an empty entry */
        r = hw_ref_add(g, id, kind, prop, required);
        if (!r)
            return;
        r->target = t;
        r->handle = ph;
        if (t == HW_NONE)
            return;                         /* its cells can't be known */
        n = cells ? prop_u32(g, t, cells, 0) : 0;
        if (o + 4 * n > len) {
            g->broken++;
            return;
        }
        r->ncells = (uint8_t)(n < HW_CELLS ? n : HW_CELLS);
        for (uint32_t i = 0; i < r->ncells; i++)
            r->cells[i] = be32(p + o + 4 * i);
        o += 4 * n;
    }
}

static const struct {
    const char *prop;
    enum hw_ref_kind kind;
    const char *cells;
    bool required;
} lists[] = {
    { "clocks",         HW_REF_CLOCK,   "#clock-cells",         true },
    { "resets",         HW_REF_RESET,   "#reset-cells",         true },
    { "power-domains",  HW_REF_POWER,   "#power-domain-cells",  true },
    { "phys",           HW_REF_PHY,     "#phy-cells",           true },
    { "pinctrl-0",      HW_REF_PINCTRL, nullptr,                true },
    { "dmas",           HW_REF_DMA,     "#dma-cells",           false },
    { "iommus",         HW_REF_IOMMU,   "#iommu-cells",         false },
    { "msi-parent",     HW_REF_MSI,     "#msi-cells",           false },
    { "nvmem-cells",    HW_REF_NVMEM,   nullptr,                false },
};

/* msi-map and iommu-map: (rid-base, phandle, base, length) entries,
 * mapping a PCI requester ID to an MSI controller or an IOMMU. */
static void id_map(struct hw_graph *g, hw_id id, const char *prop, const uint8_t *p, uint32_t len,
                   enum hw_ref_kind kind)
{
    for (uint32_t o = 0; o + 16 <= len; o += 16) {
        uint32_t ph = be32(p + o + 4);
        bool seen = false;
        struct hw_ref *r;
        struct hw_res *m;

        if (kind == HW_REF_MSI && (m = hw_res_add(g, id))) {
            m->kind = HW_MSIMAP;
            m->base = be32(p + o);
            m->size = be32(p + o + 12);
            m->ncells = 1;
            m->cells[0] = be32(p + o + 8);
            m->ctlr = by_phandle(g, ph);
        }

        for (unsigned i = 0; i < g->nodes[id].nrefs; i++)
            seen |= g->nodes[id].refs[i].kind == kind && g->nodes[id].refs[i].handle == ph;
        if (seen || !(r = hw_ref_add(g, id, kind, prop, false)))
            continue;
        r->target = by_phandle(g, ph);
        r->handle = ph;
    }
}

static void read_refs(struct hw_graph *g, hw_id id)
{
    uint32_t off, next, len;
    const char *name;
    const uint8_t *v;

    for (off = g->nodes[id].fdt_props;; off = next) {
        uint32_t t = tok(g, off);
        bool known = false;

        if (t == NOP) {
            next = off + 4;
            continue;
        }
        if (t != PROP || !prop_at(g, off, &name, &v, &len, &next))
            break;
        for (unsigned i = 0; i < sizeof lists / sizeof lists[0]; i++)
            if (hw_streq(name, lists[i].prop)) {
                phandle_list(g, id, name, v, len, lists[i].kind, lists[i].cells, lists[i].required);
                known = true;
            }
        if (known)
            continue;
        if (hw_streq(name, "msi-map"))
            id_map(g, id, name, v, len, HW_REF_MSI);
        else if (hw_streq(name, "iommu-map"))
            id_map(g, id, name, v, len, HW_REF_IOMMU);
        else if (ends_with(name, "-supply"))
            phandle_list(g, id, name, v, len, HW_REF_SUPPLY, nullptr, true);
        else if (hw_streq(name, "gpios") || hw_streq(name, "gpio") || ends_with(name, "-gpios") ||
                 ends_with(name, "-gpio"))
            phandle_list(g, id, name, v, len, HW_REF_GPIO, "#gpio-cells", true);
    }
}

/* Each reference's supplier: the device its target belongs to. A device
 * naming itself or a part of itself (a pin controller's own pins) needs
 * nothing, and nor does one on a bus that isn't a device (/cpus). A
 * target that belongs to no device stays, to be reported. */
static void resolve(struct hw_graph *g, hw_id id)
{
    struct hw_node *n = &g->nodes[id];
    unsigned keep = 0;

    for (unsigned i = 0; i < n->nrefs; i++) {
        struct hw_ref r = n->refs[i];

        r.supplier = hw_device_of(g, r.target);
        if (r.supplier == id && r.target != HW_NONE)
            continue;
        if (r.kind == HW_REF_PARENT && r.supplier == HW_NONE)
            continue;
        n->refs[keep++] = r;
    }
    n->nrefs = (uint16_t)keep;
}

/* --- The console. --- */

/* A path, or an alias and the path after it (§3.3). */
static hw_id by_path(const struct hw_graph *g, const char *path, size_t len)
{
    hw_id aliases;
    uint32_t off, next, alen;
    const char *pn;
    const uint8_t *v;
    size_t k = 0;

    if (len > 0 && path[0] == '/')
        return hw_find_path(g, path, len);
    while (k < len && path[k] != '/')
        k++;
    if (k == 0 || (aliases = hw_child(g, 0, "aliases", 7)) == HW_NONE)
        return HW_NONE;
    for (off = g->nodes[aliases].fdt_props;; off = next) {
        if (tok(g, off) == NOP) {
            next = off + 4;
            continue;
        }
        if (tok(g, off) != PROP || !prop_at(g, off, &pn, &v, &alen, &next))
            return HW_NONE;
        if (hw_strneq(path, pn, k) && alen > 0 && v[alen - 1] == 0 && v[0] == '/')
            return k == len ? hw_find_path(g, (const char *)v, alen - 1) : HW_NONE;
    }
}

static void find_console(struct hw_graph *g)
{
    hw_id chosen = hw_child(g, 0, "chosen", 6);
    uint32_t len;
    const char *s;
    size_t n = 0;

    if (chosen == HW_NONE)
        return;
    s = hw_fdt_prop(g, chosen, "stdout-path", &len);
    if (!s)
        s = hw_fdt_prop(g, chosen, "linux,stdout-path", &len);
    if (!s || len == 0 || s[len - 1] != 0)
        return;
    while (s[n] && s[n] != ':')
        n++;
    g->console = by_path(g, s, n);
}

/* --- The source. --- */

bool hw_from_fdt(struct hw_graph *g, const void *blob, uint32_t size)
{
    const uint8_t *b = blob;
    uint32_t total, version;

    if (size < 40 || be32(b) != FDT_MAGIC)
        return false;
    total = be32(b + 4);
    version = be32(b + 20);
    g->fdt_struct = be32(b + 8);
    g->fdt_strings = be32(b + 12);
    g->fdt_strings_size = be32(b + 32);
    g->fdt_struct_size = be32(b + 36);
    if (total > size || version < 17 || g->fdt_struct > total ||
        g->fdt_struct_size > total - g->fdt_struct || g->fdt_struct_size < 4 ||
        g->fdt_strings > total || g->fdt_strings_size > total - g->fdt_strings)
        return false;
    g->fdt = b;
    if (!build_nodes(g) || !index_phandles(g))
        return false;
    if (g->nodes[0].nkeys)
        g->board = g->nodes[0].keys[0];
    for (hw_id i = 0; i < g->count; i++) {
        if (g->nodes[i].nkeys == 0)
            continue;
        read_reg(g, i);
        read_pci_host(g, i);
        read_interrupts(g, i);
        read_refs(g, i);
        if (g->nodes[i].parent != HW_NONE && g->nodes[i].parent != 0) {
            struct hw_ref *r = hw_ref_add(g, i, HW_REF_PARENT, "parent", true);

            if (r)
                r->target = g->nodes[i].parent;
        }
        resolve(g, i);
    }
    find_console(g);
    return !g->full;
}
