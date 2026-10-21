/*
 * servers/hwdb/graph.c -- nodes, their parts, and the small helpers
 *
 * The arena only grows, so a list that outgrows its space is copied into
 * one twice the size and the old space is left behind. The graph is
 * built once, so that costs little.
 */
#include "hwdb.h"

void *hw_alloc(struct hw_arena *a, size_t n)
{
    size_t at = (a->used + 7) & ~(size_t)7;
    uint8_t *p;

    if (at > a->size || n > a->size - at)
        return nullptr;
    a->used = at + n;
    p = a->base + at;
    for (size_t i = 0; i < n; i++)
        p[i] = 0;
    return p;
}

/* Make room for one more of 'count' items of 'size' bytes in '*list',
 * whose space holds 'cap'; false (and the graph full) if there's none. */
static bool grow(struct hw_graph *g, void **list, uint16_t count, uint16_t *cap, size_t size)
{
    uint16_t want;
    uint8_t *n;

    if (count < *cap)
        return true;
    if (*cap >= UINT16_MAX / 2) {
        g->broken++;
        return false;
    }
    want = *cap ? (uint16_t)(*cap * 2) : 4;
    n = hw_alloc(g->arena, want * size);
    if (!n) {
        g->full = true;
        return false;
    }
    for (size_t i = 0; i < count * size; i++)
        n[i] = ((uint8_t *)*list)[i];
    *list = n;
    *cap = want;
    return true;
}

/* The capacities aren't kept: a list's is the power of two at or above
 * its count, from 4, which is what grow() makes. */
static uint16_t cap_of(uint16_t count)
{
    uint16_t c = 4;

    if (count == 0)
        return 0;
    while (c < count)
        c = (uint16_t)(c * 2);
    return c;
}

void hw_graph_init(struct hw_graph *g, struct hw_arena *a, uint32_t cap)
{
    *g = (struct hw_graph){ .arena = a, .console = HW_NONE };
    g->nodes = hw_alloc(a, cap * sizeof *g->nodes);
    g->cap = g->nodes ? cap : 0;
    g->full = !g->nodes;
}

hw_id hw_node_new(struct hw_graph *g, hw_id parent, const char *name)
{
    struct hw_node *n;

    if (g->count == g->cap) {
        g->full = true;
        return HW_NONE;
    }
    n = &g->nodes[g->count];
    *n = (struct hw_node){
        .id = g->count, .parent = parent, .name = name, .enabled = true,
        .driver = HW_NODRV,
    };
    return g->count++;
}

struct hw_res *hw_res_add(struct hw_graph *g, hw_id node)
{
    struct hw_node *n = &g->nodes[node];
    uint16_t cap = cap_of(n->nres);
    struct hw_res *r;

    if (!grow(g, (void **)&n->res, n->nres, &cap, sizeof *n->res))
        return nullptr;
    r = &n->res[n->nres++];
    *r = (struct hw_res){ .ctlr = HW_NONE };
    return r;
}

struct hw_ref *hw_ref_add(struct hw_graph *g, hw_id node, enum hw_ref_kind kind,
                          const char *name, bool required)
{
    struct hw_node *n = &g->nodes[node];
    uint16_t cap = cap_of(n->nrefs);
    struct hw_ref *r;

    if (!grow(g, (void **)&n->refs, n->nrefs, &cap, sizeof *n->refs))
        return nullptr;
    r = &n->refs[n->nrefs++];
    *r = (struct hw_ref){
        .kind = kind, .required = required, .target = HW_NONE, .supplier = HW_NONE,
        .name = name,
    };
    return r;
}

bool hw_key_add(struct hw_graph *g, hw_id node, const char *key)
{
    struct hw_node *n = &g->nodes[node];
    uint16_t cap = cap_of(n->nkeys);

    if (!grow(g, (void **)&n->keys, n->nkeys, &cap, sizeof *n->keys))
        return false;
    n->keys[n->nkeys++] = key;
    return true;
}

bool hw_has_key(const struct hw_node *n, const char *key)
{
    for (unsigned i = 0; i < n->nkeys; i++)
        if (hw_streq(n->keys[i], key))
            return true;
    return false;
}

hw_id hw_device_of(const struct hw_graph *g, hw_id target)
{
    while (target != HW_NONE && g->nodes[target].nkeys == 0)
        target = g->nodes[target].parent;
    return target != HW_NONE && g->nodes[target].parent != HW_NONE ? target : HW_NONE;
}

const char *hw_ref_kind_name(enum hw_ref_kind kind)
{
    static const char *const names[HW_REF_KINDS] = {
        [HW_REF_PARENT] = "parent", [HW_REF_IRQ] = "interrupts", [HW_REF_CLOCK] = "clock",
        [HW_REF_RESET] = "reset", [HW_REF_POWER] = "power domain", [HW_REF_SUPPLY] = "supply",
        [HW_REF_PHY] = "phy", [HW_REF_PINCTRL] = "pins", [HW_REF_GPIO] = "gpio",
        [HW_REF_IOMMU] = "iommu", [HW_REF_MSI] = "msi", [HW_REF_DMA] = "dma",
        [HW_REF_NVMEM] = "nvmem",
    };

    return kind < HW_REF_KINDS ? names[kind] : "?";
}

/* Not only after its parent: acpid's namespace moves static nodes
 * under parents made later (namespace.c). */
hw_id hw_child(const struct hw_graph *g, hw_id parent, const char *name, size_t len)
{
    for (hw_id i = 1; i < g->count; i++)
        if (g->nodes[i].parent == parent && hw_strneq(name, g->nodes[i].name, len))
            return i;
    return HW_NONE;
}

hw_id hw_find_path(const struct hw_graph *g, const char *path, size_t len)
{
    hw_id n = 0;
    size_t i = 0;

    if (g->count == 0 || len == 0 || path[0] != '/')
        return HW_NONE;
    while (i < len) {
        size_t start;

        while (i < len && path[i] == '/')
            i++;
        start = i;
        while (i < len && path[i] != '/')
            i++;
        if (i > start && (n = hw_child(g, n, path + start, i - start)) == HW_NONE)
            return HW_NONE;
    }
    return n;
}

/* --- Strings. --- */

size_t hw_strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

bool hw_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

bool hw_strneq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i] || !b[i])
            return false;
    return b[n] == 0;
}

const char *hw_strdup(struct hw_graph *g, const char *s, size_t n)
{
    char *d = hw_alloc(g->arena, n + 1);

    if (!d) {
        g->full = true;
        return "?";
    }
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return d;
}

/* --- Lines. --- */

void hw_put(struct hw_line *l, const char *s)
{
    while (*s && l->len < sizeof l->text - 1)
        l->text[l->len++] = *s++;
    l->text[l->len] = 0;
}

void hw_put_hex(struct hw_line *l, uint64_t v)
{
    char buf[17];
    unsigned i = sizeof buf - 1;

    buf[i] = 0;
    do {
        buf[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    hw_put(l, "0x");
    hw_put(l, buf + i);
}

void hw_put_dec(struct hw_line *l, uint64_t v)
{
    char buf[21];
    unsigned i = sizeof buf - 1;

    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    hw_put(l, buf + i);
}

void hw_put_path(struct hw_line *l, const struct hw_graph *g, hw_id node)
{
    hw_id chain[32];
    unsigned depth = 0;

    if (node == HW_NONE) {
        hw_put(l, "?");
        return;
    }
    for (hw_id n = node; n != HW_NONE && g->nodes[n].parent != HW_NONE && depth < 32;
         n = g->nodes[n].parent)
        chain[depth++] = n;
    if (depth == 0)
        hw_put(l, "/");
    while (depth > 0) {
        hw_put(l, "/");
        hw_put(l, g->nodes[chain[--depth]].name);
    }
}
