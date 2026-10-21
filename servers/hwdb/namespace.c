/*
 * servers/hwdb/namespace.c -- acpid's devices in the graph (docs §9.4,
 * docs/v4-plan.md step 4)
 *
 * hw_from_acpi built the graph from the static tables. acpid then sends
 * each device the namespace has (planar/acpid.h), and each goes where its
 * path says: "\_SB_.PCI0" is /_SB_/PCI0, its parents made as structure
 * if acpid didn't send them first. Its keys are its _HID and _CIDs, it's
 * enabled if _STA says present, and its _CRS gives registers, interrupts
 * (on the GIC, or on the controller a resource names), GPIO pins and bus
 * addresses. A bridge's windows aren't its registers and aren't kept.
 *
 * A device a static table already has isn't added twice. The static
 * node moves to the device's path and takes its keys first:
 * - an IORT named component, by its path;
 * - a host bridge, by its _SEG and _BBN against the MCFG's;
 * - a device whose first registers are a static node's (the SPCR's or
 *   DBG2's serial port).
 * It keeps its own resources and state (the console stays the kernel's),
 * unless it had none: the IORT's then take acpid's.
 *
 * Paths a resource names are resolved by hw_ns_done, once every device
 * is in. The record came from another process: every offset and count
 * is checked, and what doesn't fit is dropped and counted in g->broken.
 */
#include <planar/acpid.h>

#include "hwdb.h"

/* A string of the record: its offset inside, and a NUL before the end. */
static const char *str(const struct AcpidDevice *d, uint16_t at, size_t *len)
{
    size_t n = 0;

    if (at >= ACPID_STRINGS)
        return nullptr;
    while (at + n < ACPID_STRINGS && d->ad_Strings[at + n])
        n++;
    if (at + n == ACPID_STRINGS)
        return nullptr;
    *len = n;
    return d->ad_Strings + at;
}

static bool seg_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* The next segment of an ACPI path at *i: its start and length (1-4
 * characters of [A-Z0-9_]); false at the end or if it isn't one. */
static bool next_seg(const char *p, size_t len, size_t *i, size_t *start, size_t *n)
{
    if (*i >= len)
        return false;
    *start = *i;
    while (*i < len && p[*i] != '.')
        (*i)++;
    *n = *i - *start;
    if (*n == 0 || *n > 4)
        return false;
    for (size_t k = *start; k < *i; k++)
        if (!seg_char(p[k]))
            return false;
    (*i)++;                         /* the dot */
    return true;
}

/* Two paths name the same object: segments compared as ACPI pads them,
 * with '_' to four ("\_SB.DPU0" is "\_SB_.DPU0"). */
static bool same_path(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i = 1, j = 1, as, an, bs, bn;

    if (alen < 2 || blen < 2 || a[0] != '\\' || b[0] != '\\')
        return false;
    for (;;) {
        bool ma = next_seg(a, alen, &i, &as, &an), mb = next_seg(b, blen, &j, &bs, &bn);

        if (!ma || !mb)
            return !ma && !mb && i >= alen && j >= blen;
        for (size_t k = 0; k < 4; k++)
            if ((k < an ? a[as + k] : '_') != (k < bn ? b[bs + k] : '_'))
                return false;
    }
}

/* The node at an ACPI path, or with 'leaf' the node its last segment is
 * in (made, with its parents, as structure) and where that segment
 * starts. HW_NONE if it isn't a path. */
static hw_id ns_node(struct hw_graph *g, const char *p, size_t len, size_t *leaf)
{
    hw_id n = 0;
    size_t i = 1, start, k;

    if (len < 2 || p[0] != '\\')
        return HW_NONE;
    while (i < len) {
        hw_id c;

        if (!next_seg(p, len, &i, &start, &k))
            return HW_NONE;
        if (leaf && i >= len) {
            *leaf = start;
            return n;
        }
        if ((c = hw_child(g, n, p + start, k)) == HW_NONE &&
            (!leaf || (c = hw_node_new(g, n, hw_strdup(g, p + start, k))) == HW_NONE))
            return HW_NONE;
        n = c;
    }
    return leaf ? HW_NONE : n;
}

/* The GIC, which a GSIV is a line of: the MADT's distributor. */
static hw_id gic_of(const struct hw_graph *g)
{
    for (hw_id i = 1; i < g->ns_first; i++)
        if (hw_has_key(&g->nodes[i], "arm,gic-v3") ||
            hw_has_key(&g->nodes[i], "arm,cortex-a15-gic"))
            return i;
    return HW_NONE;
}

static bool is_bridge(const struct hw_node *n)
{
    return hw_has_key(n, "PNP0A08") || hw_has_key(n, "PNP0A03");
}

/* The static node the device describes too, or HW_NONE. */
static hw_id twin(struct hw_graph *g, const struct AcpidDevice *d, const struct hw_node *keys,
                  const char *path, size_t plen)
{
    hw_id iort = hw_child(g, 0, "IORT", 4);
    uint64_t base = UINT64_MAX;

    for (unsigned i = 0; i < d->ad_NRes && i < ACPID_RES; i++)
        if (d->ad_Res[i].ar_Kind == ACPID_R_MMIO) {
            base = d->ad_Res[i].ar_Base;
            break;
        }
    for (hw_id i = 1; i < g->ns_first; i++) {
        const struct hw_node *n = &g->nodes[i];
        const struct hw_res *r = n->nres ? &n->res[0] : nullptr;

        if (iort != HW_NONE && n->parent == iort && n->state == HW_S_NAMESPACE &&
            same_path(n->name, hw_strlen(n->name), path, plen))
            return i;
        if (r && r->kind == HW_ECAM && is_bridge(keys) && r->cells[0] == d->ad_Seg &&
            r->cells[1] == d->ad_Bbn)
            return i;
        if (r && r->kind == HW_MMIO && r->base == base && n->parent != 0 && n->nkeys)
            return i;
    }
    return HW_NONE;
}

static struct hw_ref *ref(struct hw_graph *g, hw_id from, enum hw_ref_kind kind, const char *name,
                          hw_id to, const char *path)
{
    struct hw_ref *r = hw_ref_add(g, from, kind, name, true);

    if (r) {
        r->target = to;
        r->supplier = to == HW_NONE ? HW_NONE : hw_device_of(g, to);
        r->path = path;
    }
    return r;
}

/* The device's resources; only its windows if 'windows' (a static twin
 * has the rest). */
static void resources(struct hw_graph *g, hw_id id, const struct AcpidDevice *d, bool windows)
{
    hw_id gic = gic_of(g);
    bool on_gic = false;

    for (unsigned i = 0; i < d->ad_NRes && i < ACPID_RES; i++) {
        const struct AcpidRes *a = &d->ad_Res[i];
        const char *src = nullptr;
        struct hw_res *r;
        struct hw_ref *f;
        size_t n = 0;

        if (windows && a->ar_Kind != ACPID_R_WINDOW)
            continue;
        if (a->ar_Source && !(src = str(d, a->ar_Source, &n))) {
            g->broken++;
            continue;
        }
        if (src)
            src = hw_strdup(g, src, n);
        switch (a->ar_Kind) {
        case ACPID_R_MMIO:
            if (a->ar_Size == 0 || a->ar_Base + a->ar_Size < a->ar_Base ||
                !(r = hw_res_add(g, id))) {
                g->broken++;
                break;
            }
            r->kind = HW_MMIO;
            r->base = a->ar_Base;
            r->size = a->ar_Size;
            break;
        case ACPID_R_IRQ:
            if (!(r = hw_res_add(g, id)))
                break;
            r->kind = HW_IRQ;
            r->ncells = 1;
            r->cells[0] = (uint32_t)a->ar_Base;
            if (src) {              /* a line of the controller it names: hw_ns_done */
                r->ctlr = HW_NONE;
                r->base = HW_NOLINE;
                if ((f = ref(g, id, HW_REF_IRQ, "interrupts", HW_NONE, src)))
                    f->cells[0] = (uint32_t)a->ar_Base, f->ncells = 1;
            } else {                /* a GSIV: the GIC's line */
                r->ctlr = gic;
                r->base = gic == HW_NONE ? HW_NOLINE : a->ar_Base;
                if (gic != HW_NONE && !on_gic)
                    ref(g, id, HW_REF_IRQ, "interrupts", gic, nullptr);
                on_gic = true;
            }
            break;
        case ACPID_R_GPIO:
            if (!src || !(f = ref(g, id, HW_REF_GPIO, "gpio", HW_NONE, src))) {
                g->broken++;
                break;
            }
            f->ncells = 2;
            f->cells[0] = (uint32_t)a->ar_Base;
            f->cells[1] = (uint32_t)a->ar_Size;
            break;
        case ACPID_R_BUS:
            if (!src || !(r = hw_res_add(g, id))) {
                g->broken++;
                break;
            }
            r->kind = HW_BUSADDR;
            r->base = a->ar_Base;
            ref(g, id, HW_REF_PARENT, "bus", HW_NONE, src);
            break;
        case ACPID_R_WINDOW:        /* memory only (acpid drops I/O); taken as identity on the bus */
            if (a->ar_Size == 0 || a->ar_Base + a->ar_Size < a->ar_Base ||
                !(r = hw_res_add(g, id))) {
                g->broken++;
                break;
            }
            r->kind = HW_WINDOW;
            r->base = a->ar_Base;
            r->size = a->ar_Size;
            r->ncells = 3;
            r->cells[0] = HW_WIN_MEM | (a->ar_Base + a->ar_Size > 1ULL << 32 ? HW_WIN_64 : 0);
            r->cells[1] = (uint32_t)(a->ar_Base >> 32);
            r->cells[2] = (uint32_t)a->ar_Base;
            break;
        default:
            g->broken++;
            break;
        }
    }
}

/* Where a static node was, for quirks that name it so. */
static void moved(struct hw_graph *g, hw_id t)
{
    struct hw_line l = { 0 };

    if (g->nmoves == g->maxmoves) {
        uint32_t want = g->maxmoves ? 2 * g->maxmoves : 16;
        struct hw_move *m = hw_alloc(g->arena, want * sizeof *m);

        if (!m) {
            g->full = true;
            return;
        }
        for (uint32_t i = 0; i < g->nmoves; i++)
            m[i] = g->moves[i];
        g->moves = m;
        g->maxmoves = want;
    }
    hw_put_path(&l, g, t);
    g->moves[g->nmoves++] = (struct hw_move){ hw_strdup(g, l.text, l.len), t };
}

hw_id hw_find_moved(const struct hw_graph *g, const char *path)
{
    for (uint32_t i = 0; i < g->nmoves; i++)
        if (hw_streq(g->moves[i].from, path))
            return g->moves[i].node;
    return HW_NONE;
}

/* Its keys, the device's first and then any it had. */
static void take_keys(struct hw_graph *g, hw_id id, const struct AcpidDevice *d)
{
    struct hw_node *n = &g->nodes[id];
    const char **old = n->keys;
    uint16_t nold = n->nkeys;

    n->keys = nullptr;
    n->nkeys = 0;
    for (unsigned i = 0; i < d->ad_NKeys && i < ACPID_KEYS; i++) {
        size_t len;
        const char *k = str(d, d->ad_Keys[i], &len);

        if (!k || len == 0)
            g->broken++;
        else if (!hw_has_key(&g->nodes[id], (k = hw_strdup(g, k, len))))
            hw_key_add(g, id, k);
    }
    for (unsigned i = 0; i < nold; i++)
        if (!hw_has_key(&g->nodes[id], old[i]))
            hw_key_add(g, id, old[i]);
}

void hw_ns_add(struct hw_graph *g, const struct AcpidDevice *d)
{
    struct hw_node keys = { 0 };
    const char *k[ACPID_KEYS], *path;
    size_t plen, leaf, len;
    hw_id id, parent, t;

    if (!g->ns_first)
        g->ns_first = g->count;
    if (!(path = str(d, d->ad_Path, &plen)) ||
        (parent = ns_node(g, path, plen, &leaf)) == HW_NONE) {
        g->broken++;
        return;
    }
    id = hw_child(g, parent, path + leaf, plen - leaf);

    /* Its keys, to look for its twin by. */
    for (unsigned i = 0; i < d->ad_NKeys && i < ACPID_KEYS; i++)
        if ((k[keys.nkeys] = str(d, d->ad_Keys[i], &len)) && len)
            keys.nkeys++;
    keys.keys = k;

    if ((t = twin(g, d, &keys, path, plen)) != HW_NONE && id == HW_NONE) {
        /* The static node moves to the device's path. */
        moved(g, t);
        g->nodes[t].parent = parent;
        g->nodes[t].name = hw_strdup(g, path + leaf, plen - leaf);
        take_keys(g, t, d);
        if (g->nodes[t].state == HW_S_NAMESPACE) {
            g->nodes[t].state = HW_S_STRUCTURE;
            g->nodes[t].enabled = d->ad_Status & 1;
        }
        resources(g, t, d, g->nodes[t].nres != 0);
        id = t;
    } else {
        if (id == HW_NONE &&
            (id = hw_node_new(g, parent, hw_strdup(g, path + leaf, plen - leaf))) == HW_NONE)
            return;
        take_keys(g, id, d);
        g->nodes[id].enabled = d->ad_Status & 1;
        resources(g, id, d, false);
    }
    /* On its parent's bus; a reservation (PNP0C02) is on none. */
    if (g->nodes[id].nkeys && hw_device_of(g, parent) != HW_NONE &&
        !hw_has_key(&g->nodes[id], "PNP0C02"))
        ref(g, id, HW_REF_PARENT, "parent", parent, nullptr);
}

void hw_ns_done(struct hw_graph *g)
{
    for (hw_id i = 1; i < g->count; i++) {
        struct hw_node *n = &g->nodes[i];
        unsigned keep = 0;

        for (unsigned k = 0; k < n->nrefs; k++) {
            struct hw_ref r = n->refs[k];

            if (r.path && r.target == HW_NONE) {
                r.target = ns_node(g, r.path, hw_strlen(r.path), nullptr);
                r.supplier = hw_device_of(g, r.target);
                /* A line of that controller: the resource's too. */
                if (r.kind == HW_REF_IRQ && r.target != HW_NONE)
                    for (unsigned q = 0; q < n->nres; q++)
                        if (n->res[q].kind == HW_IRQ && n->res[q].ctlr == HW_NONE &&
                            n->res[q].cells[0] == r.cells[0])
                            n->res[q].ctlr = r.target;
            }
            if (r.supplier == i && r.target != HW_NONE)
                continue;               /* itself */
            if (r.kind == HW_REF_PARENT && r.supplier == HW_NONE && r.target != HW_NONE)
                continue;               /* on a bus that isn't a device */
            n->refs[keep++] = r;
        }
        n->nrefs = (uint16_t)keep;
    }
}
