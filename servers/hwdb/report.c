/*
 * servers/hwdb/report.c -- the "what blocks whom" report (§11.5)
 *
 * A line per device: its path, its first key, and where it is. A node
 * that can't bind names each supplier it waits for and that supplier's
 * own state, so a chain can be followed to its end: a supplier with no
 * driver, a disabled one, a phandle that names nothing, or a cycle.
 * With detail, each device's resources and references follow. Then the
 * leases that serve several devices (lease.c), and a count.
 */
#include "hwdb.h"

static const char *state_name(enum hw_state s)
{
    switch (s) {
    case HW_S_DISABLED:  return "disabled";
    case HW_S_KERNEL:    return "kernel";
    case HW_S_FIRMWARE:  return "firmware";
    case HW_S_NAMESPACE: return "namespace";
    case HW_S_NODRIVER:  return "no driver";
    case HW_S_WAITING:   return "waiting";
    case HW_S_READY:     return "ready";
    case HW_S_BOUND:     return "bound";
    case HW_S_DECLINED:  return "declined";
    case HW_S_FAILED:    return "failed";
    default:             return "?";
    }
}

static bool is_met(const struct hw_graph *g, hw_id s)
{
    return s != HW_NONE && (g->nodes[s].state == HW_S_KERNEL ||
                            g->nodes[s].state == HW_S_FIRMWARE || g->nodes[s].state == HW_S_BOUND);
}

/* Two references to the same thing: the same supplier, or, with none,
 * the same target or phandle. */
static bool same(const struct hw_ref *a, const struct hw_ref *b)
{
    if (a->supplier != HW_NONE || b->supplier != HW_NONE)
        return a->supplier == b->supplier;
    return a->target == b->target && a->handle == b->handle &&
           (a->path == b->path || (a->path && b->path && hw_streq(a->path, b->path)));
}

static bool shown(const struct hw_graph *g, const struct hw_ref *r, bool unmet)
{
    return !unmet || (!r->dropped && !is_met(g, r->supplier));
}

/* The suppliers of n's references, each once, with what n needs of it:
 * those not met (and not dropped) if 'unmet', else all. */
static void suppliers(struct hw_line *l, const struct hw_graph *g, const struct hw_node *n, bool unmet)
{
    bool first = true;

    for (unsigned i = 0; i < n->nrefs; i++) {
        const struct hw_ref *r = &n->refs[i];
        bool seen = false;

        if (!shown(g, r, unmet))
            continue;
        for (unsigned k = 0; k < i && !seen; k++)
            seen = same(&n->refs[k], r) && shown(g, &n->refs[k], unmet);
        if (seen)
            continue;
        hw_put(l, first ? "" : "; ");
        first = false;
        if (r->target == HW_NONE && r->path) {
            hw_put(l, r->path);
            hw_put(l, ", which names no device");
        } else if (r->target == HW_NONE) {
            hw_put(l, "phandle ");
            hw_put_hex(l, r->handle);
            hw_put(l, ", which names no node");
        } else if (r->supplier == HW_NONE) {
            hw_put_path(l, g, r->target);
            hw_put(l, ", which no device provides");
        } else {
            hw_put_path(l, g, r->supplier);
        }
        hw_put(l, " (");
        for (unsigned k = i, any = 0; k < n->nrefs; k++) {
            const struct hw_ref *p = &n->refs[k];
            bool again = false;

            if (!same(p, r) || !shown(g, p, unmet))
                continue;
            for (unsigned q = i; q < k && !again; q++)
                again = same(&n->refs[q], r) && shown(g, &n->refs[q], unmet) &&
                        n->refs[q].kind == p->kind && n->refs[q].dropped == p->dropped;
            if (again)
                continue;
            hw_put(l, any++ ? ", " : "");
            hw_put(l, hw_ref_kind_name(p->kind));
            if (!p->required)
                hw_put(l, p->dropped ? " dropped" : " optional");
        }
        hw_put(l, ")");
        if (unmet && r->supplier != HW_NONE) {
            hw_put(l, ": ");
            hw_put(l, state_name(g->nodes[r->supplier].state));
        }
    }
}

static void resources(const struct hw_graph *g, const struct hw_node *n, hw_print *out, void *ctx)
{
    for (unsigned i = 0; i < n->nres; i++) {
        const struct hw_res *r = &n->res[i];
        struct hw_line l = { 0 };

        switch (r->kind) {
        case HW_MMIO:
        case HW_ECAM:
            hw_put(&l, r->kind == HW_MMIO ? "  mmio " : "  ecam ");
            hw_put_hex(&l, r->base);
            hw_put(&l, "+");
            hw_put_hex(&l, r->size);
            if (r->kind == HW_ECAM) {
                hw_put(&l, " segment ");
                hw_put_dec(&l, r->cells[0]);
                hw_put(&l, " buses ");
                hw_put_dec(&l, r->cells[1]);
                hw_put(&l, "-");
                hw_put_dec(&l, r->cells[2]);
            }
            break;
        case HW_BUSADDR:
            hw_put(&l, "  at ");
            hw_put_hex(&l, r->base);
            hw_put(&l, " on its bus");
            break;
        case HW_IRQ:
            hw_put(&l, "  irq ");
            if (r->base == HW_NOLINE) {
                hw_put(&l, "<");
                for (unsigned k = 0; k < r->ncells; k++) {
                    hw_put(&l, k ? " " : "");
                    hw_put_hex(&l, r->cells[k]);
                }
                hw_put(&l, ">");
            } else {
                hw_put_dec(&l, r->base);
            }
            hw_put(&l, " on ");
            hw_put_path(&l, g, r->ctlr);
            break;
        case HW_WINDOW:
            hw_put(&l, r->cells[0] & HW_WIN_MEM ? "  window mem" : "  window io");
            hw_put(&l, r->cells[0] & HW_WIN_64 ? "64 " : r->cells[0] & HW_WIN_MEM ? "32 " : " ");
            hw_put_hex(&l, r->base);
            hw_put(&l, "+");
            hw_put_hex(&l, r->size);
            if (r->cells[0] & HW_WIN_PREFETCH)
                hw_put(&l, " prefetchable");
            if (((uint64_t)r->cells[1] << 32 | r->cells[2]) != r->base) {
                hw_put(&l, " at ");
                hw_put_hex(&l, (uint64_t)r->cells[1] << 32 | r->cells[2]);
                hw_put(&l, " on the bus");
            }
            break;
        case HW_MSIMAP:
            hw_put(&l, "  msi ");
            hw_put_hex(&l, r->base);
            hw_put(&l, "+");
            hw_put_hex(&l, r->size);
            hw_put(&l, " as ");
            hw_put_hex(&l, r->cells[0]);
            hw_put(&l, " on ");
            hw_put_path(&l, g, r->ctlr);
            break;
        case HW_MSIX:
            hw_put(&l, "  msi-x ");
            hw_put_dec(&l, r->size);
            hw_put(&l, " vectors, table at ");
            hw_put_hex(&l, r->base);
            break;
        }
        out(ctx, l.text);
    }
    if (n->dma_limit) {
        struct hw_line l = { 0 };

        hw_put(&l, "  dma below ");
        hw_put_hex(&l, n->dma_limit);
        out(ctx, l.text);
    }
}

/* The leases serving more than one device: the pages they shared. */
static void leases(const struct hw_graph *g, hw_print *out, void *ctx)
{
    for (uint32_t i = 0; i < g->nleases; i++) {
        const struct hw_lease *ls = &g->leases[i];
        struct hw_line l = { 0 };

        if (ls->nnodes < 2)
            continue;
        hw_put(&l, "hwdb: one ");
        hw_put(&l, g->drivers[ls->driver].name);
        hw_put(&l, " for ");
        hw_put_dec(&l, ls->nnodes);
        hw_put(&l, " devices sharing");
        for (unsigned k = 0; k < ls->nranges; k++) {
            hw_put(&l, k ? ", " : " ");
            hw_put_hex(&l, ls->ranges[k].base);
            hw_put(&l, "+");
            hw_put_hex(&l, ls->ranges[k].size);
        }
        hw_put(&l, ": ");
        for (unsigned k = 0; k < ls->nnodes; k++) {
            hw_put(&l, k ? " " : "");
            hw_put(&l, g->nodes[ls->nodes[k]].name);
        }
        out(ctx, l.text);
    }
}

void hw_report(const struct hw_graph *g, bool detail, hw_print *out, void *ctx)
{
    unsigned counts[HW_S_STATES] = { 0 };
    struct hw_line l = { 0 };

    for (hw_id i = 1; i < g->count; i++) {
        const struct hw_node *n = &g->nodes[i];

        if (n->nkeys == 0 && n->state != HW_S_NAMESPACE)
            continue;
        counts[n->state]++;
        l = (struct hw_line){ 0 };
        hw_put_path(&l, g, i);
        if (n->nkeys) {
            hw_put(&l, " [");
            hw_put(&l, n->keys[0]);
            hw_put(&l, "]");
        }
        hw_put(&l, ": ");
        switch (n->state) {
        case HW_S_KERNEL:
        case HW_S_FIRMWARE:
            hw_put(&l, state_name(n->state));
            hw_put(&l, ", ");
            hw_put(&l, n->why);
            break;
        case HW_S_BOUND:
            hw_put(&l, g->drivers[n->driver].name);
            hw_put(&l, ", bound ");
            hw_put_dec(&l, n->order);
            break;
        case HW_S_WAITING:
        case HW_S_READY:
            hw_put(&l, g->drivers[n->driver].name);
            hw_put(&l, n->state == HW_S_READY ? ", ready" : ", waits for ");
            if (n->state == HW_S_WAITING)
                suppliers(&l, g, n, true);
            break;
        case HW_S_NAMESPACE:
            hw_put(&l, "named by a static table; acpid finds the rest");
            break;
        case HW_S_DECLINED:
        case HW_S_FAILED:
            hw_put(&l, g->drivers[n->driver].name);
            hw_put(&l, ", ");
            hw_put(&l, state_name(n->state));
            if (n->why) {
                hw_put(&l, ": ");
                hw_put(&l, n->why);
            }
            break;
        default:
            hw_put(&l, state_name(n->state));
            break;
        }
        if (n->alias != HW_NONE) {
            hw_put(&l, "; the same device as ");
            hw_put_path(&l, g, n->alias);
        }
        out(ctx, l.text);
        if (detail && n->state != HW_S_DISABLED) {
            resources(g, n, out, ctx);
            if (n->nrefs) {
                l = (struct hw_line){ 0 };
                hw_put(&l, "  needs ");
                suppliers(&l, g, n, false);
                out(ctx, l.text);
            }
        }
    }

    leases(g, out, ctx);
    l = (struct hw_line){ 0 };
    hw_put(&l, "hwdb: ");
    hw_put(&l, g->board ? g->board : "?");
    hw_put(&l, ":");
    for (unsigned s = HW_S_DISABLED; s < HW_S_STATES; s++)
        if (counts[s]) {
            hw_put(&l, " ");
            hw_put_dec(&l, counts[s]);
            hw_put(&l, " ");
            hw_put(&l, state_name((enum hw_state)s));
            hw_put(&l, ",");
        }
    hw_put(&l, " ");
    hw_put_dec(&l, g->broken);
    hw_put(&l, " broken");
    out(ctx, l.text);
}
