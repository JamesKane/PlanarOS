/*
 * servers/hwdb/match.c -- binding nodes to drivers by their match tables
 * (§11.6), and the check that no two drivers tie
 *
 * A node's keys run from most to least specific, as the firmware wrote
 * them (a compatible list; _HID before _CID). The first key any driver
 * takes decides, and of the drivers taking it the highest rank wins. Two
 * at the same rank would be settled by table order, silently, so the
 * build refuses them instead (hw_match_ties, run by make check).
 */
#include "hwdb.h"

static uint16_t by_name(const struct hw_graph *g, const char *name)
{
    for (unsigned d = 0; d < g->ndrivers; d++)
        if (hw_streq(g->drivers[d].name, name))
            return (uint16_t)d;
    return HW_NODRV;
}

static uint16_t choose(const struct hw_graph *g, const struct hw_node *n)
{
    for (unsigned k = 0; k < n->nkeys; k++) {
        uint16_t best = HW_NODRV;
        enum hw_rank rank = 0;

        for (unsigned d = 0; d < g->ndrivers; d++) {
            if (n->exclude && hw_streq(n->exclude, g->drivers[d].name))
                continue;
            for (const struct hw_match *m = g->drivers[d].match; m->key; m++)
                if (m->rank > rank && hw_streq(m->key, n->keys[k])) {
                    best = (uint16_t)d;
                    rank = m->rank;
                }
        }
        if (best != HW_NODRV)
            return best;
    }
    return HW_NODRV;
}

static void bind_kind(const struct hw_graph *g, struct hw_node *n, const char *why)
{
    switch (g->drivers[n->driver].kind) {
    case HW_DRV_KERNEL:
        n->state = HW_S_KERNEL;
        n->why = why ? why : "driven by the kernel";
        break;
    case HW_DRV_FIRMWARE:
        n->state = HW_S_FIRMWARE;
        n->why = why ? why : "the firmware's";
        break;
    default:
        n->state = HW_S_WAITING;    /* hw_order_start says whether it's ready */
        break;
    }
}

void hw_match(struct hw_graph *g, const struct hw_driver *drivers, unsigned ndrivers)
{
    uint16_t kernel;

    g->drivers = drivers;
    g->ndrivers = ndrivers;
    kernel = by_name(g, "kernel");
    for (hw_id i = 1; i < g->count; i++) {
        struct hw_node *n = &g->nodes[i];
        uint32_t len;
        const char *type;

        if (n->nkeys == 0 || n->state != HW_S_STRUCTURE)
            continue;               /* not a device, or a quirk or the source settled it */
        if (!n->enabled) {
            n->state = HW_S_DISABLED;
            continue;
        }
        type = hw_fdt_prop(g, i, "device_type", &len);
        if (i == g->console || (type && len == 4 && hw_streq(type, "cpu")) ||
            hw_has_key(n, "ACPI0007")) {
            n->driver = kernel;
            if (kernel != HW_NODRV)
                bind_kind(g, n, i == g->console ? "the console" : "a CPU");
            continue;
        }
        n->driver = n->pin ? by_name(g, n->pin) : choose(g, n);
        if (n->driver == HW_NODRV)
            n->state = HW_S_NODRIVER;
        else
            bind_kind(g, n, nullptr);
    }
}

unsigned hw_match_ties(const struct hw_driver *drivers, unsigned n, hw_print *out, void *ctx)
{
    unsigned ties = 0;

    for (unsigned a = 0; a < n; a++)
        for (const struct hw_match *x = drivers[a].match; x->key; x++)
            for (unsigned b = a + 1; b < n; b++)
                for (const struct hw_match *y = drivers[b].match; y->key; y++)
                    if (x->rank == y->rank && hw_streq(x->key, y->key)) {
                        struct hw_line l = { 0 };

                        hw_put(&l, "hwdb: ");
                        hw_put(&l, drivers[a].name);
                        hw_put(&l, " and ");
                        hw_put(&l, drivers[b].name);
                        hw_put(&l, " both take \"");
                        hw_put(&l, x->key);
                        hw_put(&l, "\" at rank ");
                        hw_put_dec(&l, x->rank);
                        out(ctx, l.text);
                        ties++;
                    }
    return ties;
}
