/*
 * servers/hwdb/lease.c -- which registers each driver is leased (§11.4)
 *
 * A kernel lease is a run of whole pages, and no page is in two
 * (kernel/lease.c), but devices share pages: QEMU's virtio-mmio slots
 * are 0x200 apart, and the K1's system controllers share theirs with
 * each other and with the kernel's watchdog. So the leases come from the
 * pages, not the nodes:
 *
 * - Devices whose registers share a page fall in one cluster.
 * - A cluster whose devices all go to one driver is leased once, to one
 *   process of that driver, which serves every device in it (a
 *   virtio-mmio bus driver for its eight slots).
 * - A cluster that mixes drivers, or holds a device the kernel or the
 *   firmware drives, can't be leased to any of them: their process
 *   drivers' nodes fail, and the report says what they share with. On
 *   the K1 that's `power`'s job (step 7), by a quirk.
 *
 * A device with registers in two clusters joins them into one lease with
 * two ranges. Its interrupt lines go with it.
 */
#include "hwdb.h"

struct span {
    uint64_t first, last;           /* pages */
    hw_id node;
    uint32_t set;                   /* the cluster's representative span, then the lease */
};

static bool leased(const struct hw_graph *g, const struct hw_node *n)
{
    return n->driver != HW_NODRV && g->drivers[n->driver].kind == HW_DRV_PROCESS &&
           n->state == HW_S_WAITING;
}

/* Union-find over spans, by index. */
static uint32_t root(struct span *s, uint32_t i)
{
    while (s[i].set != i)
        i = s[i].set = s[s[i].set].set;
    return i;
}

static void join(struct span *s, uint32_t a, uint32_t b)
{
    a = root(s, a);
    b = root(s, b);
    if (a != b)
        s[a > b ? a : b].set = a < b ? a : b;
}

static void fail(struct hw_graph *g, hw_id id, const char *why)
{
    g->nodes[id].state = HW_S_FAILED;
    g->nodes[id].why = why;
}

/* What to name as the reason span k's node can't be leased: a node in
 * the cluster [i, j) whose pages overlap k's and that isn't its driver's
 * or hwdb's, else the cluster's 'other', else any other node in it. */
static hw_id blocker(const struct hw_graph *g, const struct span *s, uint32_t i, uint32_t j,
                     uint32_t k, hw_id other)
{
    const struct hw_node *me = &g->nodes[s[k].node];

    for (uint32_t q = i; q < j; q++) {
        const struct hw_node *n = &g->nodes[s[q].node];

        if (s[q].node == s[k].node || s[q].first > s[k].last || s[k].first > s[q].last)
            continue;
        if (leased(g, n) ? n->driver != me->driver
                         : n->driver == HW_NODRV || g->drivers[n->driver].kind != HW_DRV_HWDB)
            return s[q].node;
    }
    for (uint32_t q = i; other == s[k].node && q < j; q++)
        if (s[q].node != s[k].node)
            other = s[q].node;
    return other;
}

void hw_leases(struct hw_graph *g)
{
    struct hw_arena *a = g->arena;
    uint32_t n = 0, cap = 0, *first;    /* first: each node's first span */
    struct span *s;

    g->nleases = 0;
    for (hw_id i = 1; i < g->count; i++) {
        g->nodes[i].lease = HW_NOLEASE;
        if (g->nodes[i].nkeys && g->nodes[i].enabled)
            cap += g->nodes[i].nres;
    }
    s = hw_alloc(a, (cap ? cap : 1) * sizeof *s);
    first = hw_alloc(a, g->count * sizeof *first);
    if (!s || !first) {
        g->full = true;
        return;
    }

    /* Every device's register pages, sorted by their first. */
    for (hw_id i = 1; i < g->count; i++) {
        const struct hw_node *d = &g->nodes[i];

        first[i] = UINT32_MAX;
        if (!d->nkeys || !d->enabled || d->state == HW_S_DISABLED)
            continue;
        for (unsigned k = 0; k < d->nres; k++) {
            const struct hw_res *r = &d->res[k];
            uint32_t j = n;

            if ((r->kind != HW_MMIO && r->kind != HW_ECAM) || r->size == 0 ||
                r->base + r->size < r->base)
                continue;
            for (; j > 0 && s[j - 1].first > r->base >> 12; j--)
                s[j] = s[j - 1];
            s[j] = (struct span){ r->base >> 12, (r->base + r->size - 1) >> 12, i, 0 };
            n++;
        }
    }
    for (uint32_t i = 0; i < n; i++)
        s[i].set = i;

    /* Overlapping spans are one cluster; a node's spans are one lease. */
    for (uint32_t i = 0; i < n;) {
        uint64_t last = s[i].last;
        uint32_t j = i + 1;

        for (; j < n && s[j].first <= last; j++) {
            if (s[j].last > last)
                last = s[j].last;
            join(s, i, j);
        }
        i = j;
    }

    /* A cluster mixing drivers, or with a device no process drives. */
    for (uint32_t i = 0; i < n;) {
        uint64_t last = s[i].last;
        uint32_t j = i + 1;
        hw_id other = HW_NONE;
        uint16_t driver = HW_NODRV;
        bool mixed = false;

        for (; j < n && s[j].first <= last; j++)
            if (s[j].last > last)
                last = s[j].last;
        for (uint32_t k = i; k < j; k++) {
            const struct hw_node *d = &g->nodes[s[k].node];

            if (!leased(g, d)) {
                if (d->driver == HW_NODRV || g->drivers[d->driver].kind != HW_DRV_HWDB)
                    other = s[k].node;      /* hwdb's own (buses) take no registers */
            } else if (driver == HW_NODRV) {
                driver = d->driver;
            } else if (d->driver != driver) {
                mixed = true;
                other = s[k].node;
            }
        }
        if (driver != HW_NODRV && (mixed || other != HW_NONE))
            for (uint32_t k = i; k < j; k++)
                if (leased(g, &g->nodes[s[k].node]) || g->nodes[s[k].node].state == HW_S_FAILED) {
                    struct hw_line l = { 0 };
                    hw_id o = blocker(g, s, i, j, k, other);
                    hw_put(&l, "shares pages with ");
                    hw_put_path(&l, g, o);
                    fail(g, s[k].node, hw_strdup(g, l.text, l.len));
                }
        i = j;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (first[s[i].node] == UINT32_MAX)
            first[s[i].node] = i;
        else
            join(s, first[s[i].node], i);
    }
    /* A node failed by one cluster fails the others it joined. */
    for (uint32_t i = 0; i < n; i++)
        if (g->nodes[s[i].node].state == HW_S_FAILED)
            for (uint32_t k = 0; k < n; k++)
                if (root(s, k) == root(s, i) && leased(g, &g->nodes[s[k].node]))
                    fail(g, s[k].node, g->nodes[s[i].node].why);

    /* One lease per remaining set: its ranges, nodes and lines. */
    g->leases = hw_alloc(a, (n ? n : 1) * sizeof *g->leases);
    if (!g->leases) {
        g->full = true;
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        struct hw_node *d = &g->nodes[s[i].node];
        struct hw_lease *l;
        uint32_t r = root(s, i);

        if (!leased(g, d))
            continue;
        if (d->lease == HW_NOLEASE) {
            /* Find the lease of another node in this set, else make one. */
            uint16_t found = HW_NOLEASE;

            for (uint32_t k = 0; k < i && found == HW_NOLEASE; k++)
                if (root(s, k) == r && g->nodes[s[k].node].lease != HW_NOLEASE)
                    found = g->nodes[s[k].node].lease;
            if (found == HW_NOLEASE) {
                found = (uint16_t)g->nleases++;
                g->leases[found] = (struct hw_lease){ .driver = d->driver };
            }
            d->lease = found;
            l = &g->leases[found];
            if (l->nnodes == HW_LEASE_NODES) {
                fail(g, s[i].node, "too many devices in one lease");
                continue;
            }
            l->nodes[l->nnodes++] = s[i].node;
            for (unsigned k = 0; k < d->nres; k++) {
                const struct hw_res *q = &d->res[k];
                bool have = false;

                if (q->kind != HW_IRQ || q->base == HW_NOLINE)
                    continue;
                for (unsigned m = 0; m < l->nlines; m++)
                    have |= l->lines[m] == q->base;
                if (have)
                    continue;
                if (l->nlines == HW_LEASE_LINES) {
                    g->broken++;
                    continue;
                }
                l->lines[l->nlines++] = (uint32_t)q->base;
            }
        }
        l = &g->leases[d->lease];
        /* This span's pages: a range of its own, unless one holds them. */
        {
            uint64_t base = s[i].first << 12, end = (s[i].last + 1) << 12;
            bool merged = false;

            for (unsigned k = 0; k < l->nranges && !merged; k++) {
                struct hw_range *rg = &l->ranges[k];

                if (base <= rg->base + rg->size && end >= rg->base) {
                    uint64_t lo = base < rg->base ? base : rg->base;
                    uint64_t hi = end > rg->base + rg->size ? end : rg->base + rg->size;

                    rg->base = lo;
                    rg->size = hi - lo;
                    merged = true;
                }
            }
            if (!merged) {
                if (l->nranges == HW_LEASE_RANGES)
                    g->broken++;
                else
                    l->ranges[l->nranges++] = (struct hw_range){ base, end - base };
            }
        }
    }
}
