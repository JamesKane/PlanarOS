/*
 * servers/hwdb/order.c -- binding in dependency order (§11.5)
 *
 * A reference is met when its supplier is bound: by the kernel, by the
 * firmware, or by a driver hwdb started. A matched node is ready when
 * all its references are met or dropped. When a node binds, only its
 * consumers are looked at again: there's no retry loop.
 *
 * An optional reference (DMA, IOMMU, MSI, nvmem) orders its node after
 * the supplier when the supplier can bind. It's dropped when it can't: at
 * the start if the supplier is missing, disabled or has no driver, and
 * when nothing else is ready, if it's what holds a node back (a supplier
 * still waiting then waits in a cycle or on something that won't come).
 */
#include "hwdb.h"

static bool met(const struct hw_graph *g, hw_id s)
{
    if (s == HW_NONE)
        return false;
    switch (g->nodes[s].state) {
    case HW_S_KERNEL:
    case HW_S_FIRMWARE:
    case HW_S_BOUND:
        return true;
    default:
        return false;
    }
}

static bool hopeless(const struct hw_graph *g, hw_id s)
{
    if (s == HW_NONE)
        return true;
    switch (g->nodes[s].state) {
    case HW_S_DISABLED:
    case HW_S_NODRIVER:
    case HW_S_STRUCTURE:
    case HW_S_DECLINED:
    case HW_S_FAILED:
        return true;
    default:
        return false;
    }
}

static uint32_t pending(const struct hw_graph *g, const struct hw_node *n)
{
    uint32_t p = 0;

    for (unsigned i = 0; i < n->nrefs; i++)
        p += !n->refs[i].dropped && !met(g, n->refs[i].supplier);
    return p;
}

/* Look at a waiting node again: queue it if nothing holds it now. */
static void examine(struct hw_graph *g, hw_id id)
{
    struct hw_node *n = &g->nodes[id];

    if (n->state != HW_S_WAITING)
        return;
    g->examined++;
    n->pending = pending(g, n);
    if (n->pending == 0) {
        n->state = HW_S_READY;
        g->queue[g->qtail++] = id;
    }
}

static void add_consumer(struct hw_graph *g, hw_id s, hw_id c)
{
    struct hw_node *n = &g->nodes[s];
    hw_id *list;

    for (unsigned i = 0; i < n->nconsumers; i++)
        if (n->consumers[i] == c)
            return;
    if (n->nconsumers == n->maxconsumers) {
        uint16_t want = n->maxconsumers ? (uint16_t)(n->maxconsumers * 2) : 4;

        if (!(list = hw_alloc(g->arena, want * sizeof *list))) {
            g->full = true;
            return;
        }
        for (unsigned i = 0; i < n->nconsumers; i++)
            list[i] = n->consumers[i];
        n->consumers = list;
        n->maxconsumers = want;
    }
    n->consumers[n->nconsumers++] = c;
}

void hw_order_start(struct hw_graph *g)
{
    g->queue = hw_alloc(g->arena, (g->count ? g->count : 1) * sizeof *g->queue);
    g->qhead = g->qtail = g->bound = 0;
    if (!g->queue) {
        g->full = true;
        return;
    }
    for (hw_id i = 0; i < g->count; i++) {
        struct hw_node *n = &g->nodes[i];

        for (unsigned k = 0; k < n->nrefs; k++) {
            struct hw_ref *r = &n->refs[k];

            if (!r->required && hopeless(g, r->supplier))
                r->dropped = true;
            if (r->supplier != HW_NONE)
                add_consumer(g, r->supplier, i);
        }
    }
    for (hw_id i = 0; i < g->count; i++)
        examine(g, i);
}

/* Nothing is ready: drop the optional references holding nodes back. */
bool hw_unstick(struct hw_graph *g)
{
    uint32_t was = g->qtail;

    for (hw_id i = 0; i < g->count; i++) {
        struct hw_node *n = &g->nodes[i];
        bool dropped = false;

        if (n->state != HW_S_WAITING)
            continue;
        for (unsigned k = 0; k < n->nrefs; k++)
            if (!n->refs[k].required && !n->refs[k].dropped && !met(g, n->refs[k].supplier)) {
                n->refs[k].dropped = true;
                dropped = true;
            }
        if (dropped)
            examine(g, i);
    }
    return g->qtail != was;
}

hw_id hw_next(struct hw_graph *g)
{
    if (!g->queue)
        return HW_NONE;
    return g->qhead < g->qtail ? g->queue[g->qhead++] : HW_NONE;
}

void hw_bound(struct hw_graph *g, hw_id id)
{
    struct hw_node *n = &g->nodes[id];

    if (n->state != HW_S_READY)
        return;
    n->state = HW_S_BOUND;
    n->order = ++g->bound;
    for (unsigned i = 0; i < n->nconsumers; i++)
        examine(g, n->consumers[i]);
}

void hw_unbind(struct hw_graph *g, hw_id id, enum hw_state state, const char *why)
{
    g->nodes[id].state = state;
    g->nodes[id].why = why;
}

void hw_bind_all(struct hw_graph *g)
{
    hw_id n;

    do
        while ((n = hw_next(g)) != HW_NONE)
            hw_bound(g, n);
    while (hw_unstick(g));
}

void hw_prepare(struct hw_graph *g)
{
    uint16_t units[256] = { 0 };

    hw_quirks(g, hw_quirk_table, hw_nquirks);
    hw_match(g, hw_drivers, hw_ndrivers);
    for (hw_id i = 1; i < g->count; i++)
        if (g->nodes[i].driver < 256)
            g->nodes[i].unit = units[g->nodes[i].driver]++;
    hw_leases(g);
    hw_order_start(g);
}
