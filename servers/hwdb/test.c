/*
 * servers/hwdb/test.c -- hwdb's rules on small graphs made to break them
 *
 * The captured trees (make hwdb-test) show that real descriptions bind
 * as they should; these show the cases they don't have: cycles, optional
 * references dropped, a phandle that names nothing, a disabled supplier,
 * consumers looked at again and nothing else, ranks, ties and quirks,
 * and acpid's records merged into a static graph, damaged ones too.
 * A tree built here checks where devices' DMA stops (dma-ranges).
 * Then damaged blobs and tables, which must be refused or read in part,
 * never read past (run under the address sanitizer).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/acpid.h>

#include "hwdb.h"

static unsigned failures, checks;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static uint8_t space[4 << 20];
static struct hw_arena arena;
static struct hw_graph g;

static void empty(void)
{
    arena = (struct hw_arena){ space, sizeof space, 0 };
    hw_graph_init(&g, &arena, 4096);
}

/* An empty graph with a root, for building by hand. */
static void fresh(void)
{
    empty();
    hw_node_new(&g, HW_NONE, "");
}

static hw_id dev(const char *name, const char *key)
{
    hw_id n = hw_node_new(&g, 0, name);

    hw_key_add(&g, n, key);
    return n;
}

static void need(hw_id from, hw_id to, bool required)
{
    struct hw_ref *r = hw_ref_add(&g, from, HW_REF_CLOCK, "clocks", required);

    r->target = to;
    r->supplier = hw_device_of(&g, to);
}

static const struct hw_match m_a[] = { { "a", HW_SPECIFIC }, { "shared", HW_DEFAULT }, { nullptr, 0 } };
static const struct hw_match m_b[] = { { "b", HW_SPECIFIC }, { "shared", HW_VENDOR }, { nullptr, 0 } };
static const struct hw_match m_k[] = { { "k", HW_SPECIFIC }, { nullptr, 0 } };
static const struct hw_driver drivers[] = {
    { "kernel", HW_DRV_KERNEL, m_k },
    { "drv-a", HW_DRV_PROCESS, m_a },
    { "drv-b", HW_DRV_PROCESS, m_b },
};

static void run(void)
{
    hw_match(&g, drivers, 3);
    hw_order_start(&g);
    hw_bind_all(&g);
}

static void print(void *ctx, const char *line)
{
    if (ctx)
        fprintf(ctx, "%s\n", line);
}

/* The report's line for a node, for checking what it says. */
static char said[512];

static void grab(void *ctx, const char *line)
{
    const char *want = ctx;

    if (strncmp(line, want, strlen(want)) == 0 && line[strlen(want)] == ' ')
        snprintf(said, sizeof said, "%s", line);
}

static const char *line_for(const char *path)
{
    said[0] = 0;
    hw_report(&g, false, grab, (void *)path);
    return said;
}

static void test_order(void)
{
    hw_id k, a, b, c, d;

    /* A chain: c needs b needs a needs the kernel's k. */
    fresh();
    k = dev("k", "k");
    a = dev("a", "a");
    b = dev("b", "a");
    c = dev("c", "a");
    need(a, k, true);
    need(b, a, true);
    need(c, b, true);
    run();
    CHECK(g.nodes[k].state == HW_S_KERNEL);
    CHECK(g.nodes[a].order == 1 && g.nodes[b].order == 2 && g.nodes[c].order == 3);

    /* Only consumers are looked at again: a node with many consumers,
     * and one waiting on something else that never comes. */
    fresh();
    a = dev("a", "a");
    for (unsigned i = 0; i < 10; i++)
        need(dev("c", "a"), a, true);
    d = dev("d", "a");
    need(d, dev("nodrv", "zzz"), true);
    run();
    /* 12 waiting at the start, then a's 10 consumers once each. */
    CHECK(g.examined == 12 + 10);
    CHECK(g.nodes[d].state == HW_S_WAITING);
    CHECK(strstr(line_for("/d"), "waits for /nodrv (clock): no driver"));

    /* A cycle of required references: neither binds, and each says so. */
    fresh();
    a = dev("a", "a");
    b = dev("b", "a");
    need(a, b, true);
    need(b, a, true);
    run();
    CHECK(g.nodes[a].state == HW_S_WAITING && g.nodes[b].state == HW_S_WAITING);
    CHECK(strstr(line_for("/a"), "waits for /b (clock): waiting"));

    /* The same cycle with one edge optional: dropped once nothing else
     * is ready, then both bind, b first. */
    fresh();
    a = dev("a", "a");
    b = dev("b", "a");
    c = dev("c", "a");
    need(a, b, true);
    need(b, a, false);
    need(c, a, true);
    run();
    CHECK(g.nodes[b].order == 1 && g.nodes[a].order == 2 && g.nodes[c].order == 3);
    CHECK(g.nodes[b].refs[0].dropped);

    /* An optional reference to a supplier that can bind orders, and isn't
     * dropped. */
    fresh();
    b = dev("b", "a");
    a = dev("a", "a");
    need(b, a, false);
    run();
    CHECK(g.nodes[a].order == 1 && g.nodes[b].order == 2 && !g.nodes[b].refs[0].dropped);

    /* A required reference to a disabled device, or to a phandle that
     * names nothing, or to a node no device provides. */
    fresh();
    a = dev("a", "a");
    b = dev("off", "a");
    g.nodes[b].enabled = false;
    need(a, b, true);
    c = dev("c", "a");
    {
        struct hw_ref *r = hw_ref_add(&g, c, HW_REF_RESET, "resets", true);

        r->handle = 0x77;
    }
    d = dev("d", "a");
    need(d, hw_node_new(&g, 0, "plain"), true);
    run();
    CHECK(strstr(line_for("/a"), "waits for /off (clock): disabled"));
    CHECK(strstr(line_for("/c"), "waits for phandle 0x77, which names no node (reset)"));
    CHECK(strstr(line_for("/d"), "waits for /plain, which no device provides (clock)"));
}

static void test_match(void)
{
    hw_id n, m, p;
    const struct hw_match t1[] = { { "x", HW_DEFAULT }, { nullptr, 0 } };
    const struct hw_match t2[] = { { "y", HW_DEFAULT }, { "x", HW_DEFAULT }, { nullptr, 0 } };
    const struct hw_driver tied[] = { { "one", HW_DRV_PROCESS, t1 }, { "two", HW_DRV_PROCESS, t2 } };

    /* The node's first key that any driver takes decides; the rank only
     * settles a key two drivers take. */
    fresh();
    n = dev("n", "b");
    hw_key_add(&g, n, "a");
    m = dev("m", "shared");
    p = dev("p", "nothing");
    run();
    CHECK(hw_streq(drivers[g.nodes[n].driver].name, "drv-b"));
    CHECK(hw_streq(drivers[g.nodes[m].driver].name, "drv-b"));
    CHECK(g.nodes[p].state == HW_S_NODRIVER);

    /* Ties are found; the real tables have none. */
    CHECK(hw_match_ties(tied, 2, print, nullptr) == 1);
    CHECK(hw_match_ties(drivers, 3, print, nullptr) == 0);
    CHECK(hw_match_ties(hw_drivers, hw_ndrivers, print, stderr) == 0);
}

static void test_quirks(void)
{
    const struct hw_quirk q[] = {
        { "board", HW_Q_PIN, "/m", "drv-a" },
        { "board", HW_Q_EXCLUDE, "/n", "drv-b" },
        { "board", HW_Q_DISABLE, "/bus", nullptr },
        { "board", HW_Q_FIRMWARE, "/fw", "left on" },
        { "board", HW_Q_CONSOLE, "/con", nullptr },
        { "board", HW_Q_DMA_LIMIT, "/user", "0x8000F000" },
        { "other", HW_Q_DISABLE, "/m", nullptr },
        { "board", HW_Q_DISABLE, "/absent", nullptr },
    };
    hw_id n, m, bus, child, fw, con, user;

    fresh();
    g.board = "board";
    n = dev("n", "b");
    hw_key_add(&g, n, "a");
    m = dev("m", "shared");
    bus = dev("bus", "a");
    child = hw_node_new(&g, bus, "child");
    hw_key_add(&g, child, "a");
    fw = dev("fw", "zzz");
    con = dev("con", "zzz");
    user = dev("user", "a");
    need(user, fw, true);
    CHECK(hw_quirks(&g, q, sizeof q / sizeof q[0]) == 6);
    CHECK(g.nodes[user].dma_limit == 0x8000f000);
    CHECK(g.broken == 1);                           /* /absent */
    run();
    CHECK(hw_streq(drivers[g.nodes[m].driver].name, "drv-a"));
    CHECK(hw_streq(drivers[g.nodes[n].driver].name, "drv-a"));
    CHECK(g.nodes[bus].state == HW_S_DISABLED && g.nodes[child].state == HW_S_DISABLED);
    CHECK(g.nodes[fw].state == HW_S_FIRMWARE && g.nodes[user].state == HW_S_BOUND);
    CHECK(g.nodes[con].state == HW_S_KERNEL);
}

static void *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *p;

    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path);
        exit(2);
    }
    p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    fclose(f);
    *len = (uint32_t)n;
    return p;
}

static uint32_t seed = 12345;

static uint32_t rnd(void)
{
    seed = seed * 1103515245 + 12345;
    return seed >> 8;
}

/* Every source, damaged: each blob cut short at many lengths and with
 * bytes changed at random, copied to exactly its length so a read past
 * it is the sanitizer's to catch. */
/* --- A device tree built by hand (FDT v17), for what no capture has. --- */

static struct fdtb {
    uint8_t blob[8192];
    uint32_t len;                   /* of the structure block so far */
    char strings[1024];
    uint32_t nstrings;
} fb;

static void fb_u32(uint32_t v)
{
    for (int i = 3; i >= 0; i--)
        fb.blob[40 + fb.len++] = (uint8_t)(v >> (8 * i));
}

static void fb_pad(void)
{
    while (fb.len % 4)
        fb.blob[40 + fb.len++] = 0;
}

static void fb_begin(const char *name)
{
    fb_u32(1);
    memcpy(&fb.blob[40 + fb.len], name, strlen(name) + 1);
    fb.len += (uint32_t)strlen(name) + 1;
    fb_pad();
}

static void fb_prop(const char *name, const void *v, uint32_t n)
{
    uint32_t off = 0;

    while (off < fb.nstrings && strcmp(&fb.strings[off], name))
        off += (uint32_t)strlen(&fb.strings[off]) + 1;
    if (off >= fb.nstrings) {
        memcpy(&fb.strings[fb.nstrings], name, strlen(name) + 1);
        fb.nstrings += (uint32_t)strlen(name) + 1;
    }
    fb_u32(3);
    fb_u32(n);
    fb_u32(off);
    memcpy(&fb.blob[40 + fb.len], v, n);
    fb.len += n;
    fb_pad();
}

/* A property of 32-bit cells, big-endian. */
static void fb_cells(const char *name, const uint32_t *c, unsigned n)
{
    uint8_t v[64];

    for (unsigned i = 0; i < n; i++)
        for (int b = 0; b < 4; b++)
            v[4 * i + b] = (uint8_t)(c[i] >> (24 - 8 * b));
    fb_prop(name, v, 4 * n);
}

static void fb_str(const char *name, const char *s)
{
    fb_prop(name, s, (uint32_t)strlen(s) + 1);
}

static void fb_end(void)
{
    fb_u32(2);
}

static uint32_t fb_finish(void)
{
    uint32_t total, off = 40 + fb.len + 4;
    uint32_t hdr[10] = { 0xd00dfeed, 0, 40, 0, 40, 17, 16, 0, fb.nstrings, 0 };

    fb_u32(9);                      /* FDT_END */
    memcpy(&fb.blob[off], fb.strings, fb.nstrings);
    total = off + fb.nstrings;
    hdr[1] = total;
    hdr[3] = off;
    hdr[9] = fb.len;
    for (unsigned i = 0; i < 10; i++)
        for (int b = 0; b < 4; b++)
            fb.blob[4 * i + b] = (uint8_t)(hdr[i] >> (24 - 8 * b));
    return total;
}

static void fb_bus(void)
{
    static const uint32_t two[] = { 2 };

    fb_cells("#address-cells", two, 1);
    fb_cells("#size-cells", two, 1);
    fb_prop("ranges", nullptr, 0);
}

static void fb_dev(const char *name, const char *compat, uint32_t reg, int interconnect)
{
    uint32_t r[] = { 0, reg, 0, 0x1000 }, ic[] = { (uint32_t)interconnect };

    fb_begin(name);
    fb_str("compatible", compat);
    fb_cells("reg", r, 4);
    if (interconnect)
        fb_cells("interconnects", ic, 1);
    fb_end();
}

/*
 * The K1's shape: a soc bus whose dma-ranges reach 4 GiB where memory
 * is; a dram bus that also moves memory above 4 GiB down (dram_range1);
 * one that only moves it (none where it is); and a bus with no
 * dma-ranges at all.
 */
static void test_dma(void)
{
    static const uint32_t soc[] = { 0, 0, 0, 0, 1, 0 };
    static const uint32_t range1[] = { 0, 0, 0, 0, 0, 0x80000000, 0, 0x80000000, 1, 0, 3, 0x80000000 };
    static const uint32_t moved[] = { 0, 0, 1, 0, 0, 0x80000000 };
    static const uint32_t ph1[] = { 1 }, ph2[] = { 2 };
    uint32_t len;
    hw_id gpu, sd, odd, plain;

    fb = (struct fdtb){ 0 };
    fb_begin("");
    fb_str("compatible", "test,dma");
    fb_bus();
    fb_begin("soc");
    fb_str("compatible", "simple-bus");
    fb_bus();
    fb_cells("dma-ranges", soc, 6);
    fb_begin("dram@1");
    fb_cells("phandle", ph1, 1);
    fb_bus();
    fb_cells("dma-ranges", range1, 12);
    fb_end();
    fb_begin("dram@2");
    fb_cells("phandle", ph2, 1);
    fb_bus();
    fb_cells("dma-ranges", moved, 6);
    fb_end();
    fb_dev("gpu@1000", "test,gpu", 0x1000, 1);
    fb_dev("sd@2000", "test,sd", 0x2000, 0);
    fb_dev("odd@3000", "test,odd", 0x3000, 2);
    fb_end();
    fb_begin("plain");
    fb_str("compatible", "simple-bus");
    fb_bus();
    fb_dev("dev@4000", "test,plain", 0x4000, 0);
    fb_end();
    fb_end();
    len = fb_finish();

    empty();
    CHECK(hw_from_fdt(&g, fb.blob, len));
    gpu = hw_find_path(&g, "/soc/gpu@1000", 13);
    sd = hw_find_path(&g, "/soc/sd@2000", 12);
    odd = hw_find_path(&g, "/soc/odd@3000", 13);
    plain = hw_find_path(&g, "/plain/dev@4000", 15);
    CHECK(gpu != HW_NONE && g.nodes[gpu].dma_limit == 0x80000000);    /* its interconnect's */
    CHECK(sd != HW_NONE && g.nodes[sd].dma_limit == 1ULL << 32);       /* its bus's */
    CHECK(odd != HW_NONE && g.nodes[odd].dma_limit == 0x1000);         /* nowhere */
    CHECK(plain != HW_NONE && g.nodes[plain].dma_limit == 0);           /* no limit */
    CHECK(g.broken == 0);
}

static void test_damage(const char *dtb, const char *const *tables, unsigned ntables)
{
    uint32_t len;
    uint8_t *blob = slurp(dtb, &len);
    unsigned built = 0;

    for (unsigned round = 0; round < 3000; round++) {
        uint32_t n = round < 1000 ? (uint32_t)((uint64_t)len * round / 1000) : len;
        uint8_t *copy = malloc(n ? n : 1);

        memcpy(copy, blob, n);
        if (round >= 1000)
            for (unsigned k = 0; k < 1 + rnd() % 8; k++)
                copy[rnd() % n] = (uint8_t)rnd();
        empty();
        if (hw_from_fdt(&g, copy, n)) {
            built++;
            hw_prepare(&g);
            hw_bind_all(&g);
            hw_report(&g, true, print, nullptr);
        }
        free(copy);
    }
    CHECK(built > 0);
    free(blob);

    /* Tables: cut short (refused, their lengths say more); changed at
     * random; and shortened, the length and checksum mended to match, so
     * the last entry may run off the end. */
    for (unsigned round = 0; round < 4000; round++) {
        struct hw_table t[32];
        uint8_t *copies[32];

        for (unsigned i = 0; i < ntables && i < 32; i++) {
            uint8_t *data = slurp(tables[i], &len);
            uint32_t n = len;
            bool mend = false;

            if (round < 1000)
                n = len - (uint32_t)((uint64_t)len * (round % 100) / 100);
            else if (round >= 2000 && len > 36)
                n = 36 + rnd() % (len - 36), mend = true;
            copies[i] = malloc(n ? n : 1);
            memcpy(copies[i], data, n);
            free(data);
            if (round >= 1000 && round < 2000 && n > 0) {
                for (unsigned k = 0; k < 1 + rnd() % 4; k++)
                    copies[i][rnd() % n] = (uint8_t)rnd();
                mend = n >= 36 && rnd() % 4;
            }
            if (mend) {
                uint8_t sum = 0;

                if (round >= 2000)
                    for (unsigned k = 0; k < 4; k++)
                        copies[i][4 + k] = (uint8_t)(n >> 8 * k);
                for (uint32_t k = 0; k < n; k++)
                    sum += copies[i][k];
                copies[i][9] = (uint8_t)(copies[i][9] - sum);
            }
            t[i] = (struct hw_table){ copies[i], n };
        }
        empty();
        if (hw_from_acpi(&g, t, ntables)) {
            hw_prepare(&g);
            hw_bind_all(&g);
            hw_report(&g, true, print, nullptr);
        }
        for (unsigned i = 0; i < ntables && i < 32; i++)
            free(copies[i]);
    }
}

static void reg(hw_id n, enum hw_res_kind kind, uint64_t base, uint64_t size)
{
    struct hw_res *r = hw_res_add(&g, n);

    r->kind = kind;
    r->base = base;
    r->size = size;
}

/* Who is leased what (lease.c): devices sharing a page share a lease. */
static void test_leases(void)
{
    hw_id a1, a2, a3, b1, k1, b2, a4, b3, b4, c;
    const struct hw_lease *l;

    fresh();
    a1 = dev("a1", "a");                    /* two slots in a page */
    reg(a1, HW_MMIO, 0x1000, 0x200);
    reg(a1, HW_IRQ, 5, 0);
    a2 = dev("a2", "a");
    reg(a2, HW_MMIO, 0x1200, 0x200);
    reg(a2, HW_IRQ, 6, 0);
    reg(a2, HW_IRQ, 5, 0);                  /* a line they share: once */
    a3 = dev("a3", "a");                    /* a page of its own */
    reg(a3, HW_MMIO, 0x3000, 0x1000);
    b1 = dev("b1", "b");                    /* sharing with the kernel's */
    reg(b1, HW_MMIO, 0x5000, 0x100);
    k1 = dev("k1", "k");
    reg(k1, HW_MMIO, 0x5800, 0x100);
    b2 = dev("b2", "b");                    /* with another driver's */
    reg(b2, HW_MMIO, 0x7000, 0x100);
    a4 = dev("a4", "a");
    reg(a4, HW_MMIO, 0x7400, 0x100);
    b3 = dev("b3", "b");                    /* two ranges, one shared */
    reg(b3, HW_MMIO, 0x9000, 0x1000);
    reg(b3, HW_MMIO, 0xb000, 0x100);
    b4 = dev("b4", "b");
    reg(b4, HW_MMIO, 0xb100, 0x100);
    c = dev("c", "a");                      /* needs a device that can't be leased */
    need(c, b1, true);

    hw_match(&g, drivers, 3);
    hw_leases(&g);
    hw_order_start(&g);
    hw_bind_all(&g);
    CHECK(g.nleases == 3);
    l = &g.leases[g.nodes[a1].lease];
    CHECK(g.nodes[a2].lease == g.nodes[a1].lease && l->nnodes == 2 && l->nranges == 1 &&
          l->ranges[0].base == 0x1000 && l->ranges[0].size == 0x1000 && l->nlines == 2);
    l = &g.leases[g.nodes[a3].lease];
    CHECK(g.nodes[a3].lease != g.nodes[a1].lease && l->nnodes == 1 && l->nlines == 0);
    CHECK(g.nodes[b1].state == HW_S_FAILED && g.nodes[b1].lease == HW_NOLEASE);
    CHECK(strstr(line_for("/b1"), "failed: shares pages with /k1"));
    CHECK(g.nodes[k1].state == HW_S_KERNEL);
    CHECK(g.nodes[b2].state == HW_S_FAILED && g.nodes[a4].state == HW_S_FAILED);
    CHECK(strstr(line_for("/b2"), "shares pages with /a4"));
    l = &g.leases[g.nodes[b3].lease];
    CHECK(g.nodes[b4].lease == g.nodes[b3].lease && l->nnodes == 2 && l->nranges == 2 &&
          l->ranges[1].base == 0xb000 && l->ranges[1].size == 0x1000);
    CHECK(g.nodes[b3].state == HW_S_BOUND && g.nodes[b4].state == HW_S_BOUND);
    CHECK(strstr(line_for("/c"), "waits for /b1 (clock): failed"));
    CHECK(strstr(line_for("hwdb: one drv-a"), "for 2 devices sharing 0x1000+0x1000: a1 a2"));
}

/* Nodes a bus driver adds once the graph is running (hw_add_late): a
 * function behind a host bridge gets its own lease and binds after the
 * bridge; one on another device's pages fails, saying whose. */
static void test_late(void)
{
    hw_id bridge, uart, f1, f2, f3;
    struct hw_ref *r;

    fresh();
    bridge = dev("pcie", "a");
    reg(bridge, HW_ECAM, 0x40000000, 0x1000000);
    uart = dev("uart", "k");
    reg(uart, HW_MMIO, 0x10002000, 0x100);
    hw_match(&g, drivers, 3);
    hw_leases(&g);
    hw_order_start(&g);
    CHECK(hw_next(&g) == bridge);   /* ready; not yet bound */

    f1 = hw_node_new(&g, bridge, "00:01.0");
    hw_key_add(&g, f1, "b");
    reg(f1, HW_MMIO, 0x10004000, 0x4000);
    reg(f1, HW_IRQ, 8192, 0);
    reg(f1, HW_IRQ, 8193, 0);
    r = hw_ref_add(&g, f1, HW_REF_PARENT, "parent", true);
    r->target = r->supplier = bridge;
    hw_add_late(&g, f1);
    CHECK(g.nodes[f1].state == HW_S_WAITING && g.nodes[f1].lease == g.nleases - 1);
    CHECK(g.leases[g.nodes[f1].lease].nranges == 1 && g.leases[g.nodes[f1].lease].nlines == 2 &&
          g.leases[g.nodes[f1].lease].lines[1] == 8193);
    CHECK(hw_next(&g) == HW_NONE);  /* waits for the bridge */
    hw_bound(&g, bridge);
    CHECK(hw_next(&g) == f1 && g.nodes[f1].state == HW_S_READY);

    f2 = hw_node_new(&g, bridge, "00:02.0");    /* the uart's page */
    hw_key_add(&g, f2, "b");
    reg(f2, HW_MMIO, 0x10002800, 0x100);
    hw_add_late(&g, f2);
    CHECK(g.nodes[f2].state == HW_S_FAILED && g.nodes[f2].lease == HW_NOLEASE);
    CHECK(strstr(line_for("/pcie/00:02.0"), "shares pages with /uart"));
    CHECK(g.nodes[f2].unit == 1);   /* numbered all the same */

    f3 = hw_node_new(&g, bridge, "00:03.0");    /* no driver takes it */
    hw_key_add(&g, f3, "pciclass,0600");
    hw_add_late(&g, f3);
    CHECK(g.nodes[f3].state == HW_S_NODRIVER);
}

/* --- acpid's records (namespace.c). --- */

static struct AcpidDevice rec;
static unsigned rec_used;

static uint16_t rstr(const char *str)
{
    size_t n = strlen(str);
    uint16_t at = (uint16_t)rec_used;

    memcpy(rec.ad_Strings + rec_used, str, n + 1);
    rec_used += (unsigned)n + 1;
    return at;
}

static void rec_new(const char *path, uint32_t sta)
{
    memset(&rec, 0, sizeof rec);
    rec_used = 1;
    rec.ad_Status = sta;
    rec.ad_Adr = UINT64_MAX;
    rec.ad_Path = rstr(path);
}

static void rkey(const char *key)
{
    rec.ad_Keys[rec.ad_NKeys++] = rstr(key);
}

static void rres(uint8_t kind, uint64_t base, uint64_t size, const char *src)
{
    rec.ad_Res[rec.ad_NRes++] = (struct AcpidRes){
        .ar_Kind = kind, .ar_Base = base, .ar_Size = size, .ar_Source = src ? rstr(src) : 0
    };
}

static hw_id under(hw_id parent, const char *name, const char *key)
{
    hw_id n = hw_node_new(&g, parent, name);

    if (key)
        hw_key_add(&g, n, key);
    return n;
}

static void test_namespace(void)
{
    hw_id gic, uart, dpu, pci, gpio, dev0, i2c, off, iort;
    unsigned broken;
    const struct hw_node *n;

    /* What the static tables gave: the GIC, an IORT named component (its
     * path as the IORT spells it), the SPCR's console, the MCFG's bridge. */
    fresh();
    gic = under(under(0, "MADT", nullptr), "gicd@8000000", "arm,gic-v3");
    iort = under(0, "IORT", nullptr);
    dpu = under(iort, "\\_SB.DPU0", nullptr);
    g.nodes[dpu].state = HW_S_NAMESPACE;
    uart = under(under(0, "SPCR", nullptr), "serial@9000000", "arm,pl011");
    reg(uart, HW_MMIO, 0x9000000, 0x1000);
    g.console = uart;
    pci = under(under(0, "MCFG", nullptr), "pci@0,0", "PNP0A08");
    reg(pci, HW_ECAM, 0x40000000, 0x10000000);

    rec_new("\\_SB_", 0xf);
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.COM0", 0xf);
    rkey("ARMH0011");
    rres(ACPID_R_MMIO, 0x9000000, 0x1000, nullptr);
    rres(ACPID_R_IRQ, 33, 1, nullptr);
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.DPU0", 0xf);
    rkey("CIXH5010");
    rres(ACPID_R_MMIO, 0x14000000, 0x10000, nullptr);
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.PCI0", 0xf);
    rkey("PNP0A08");
    rkey("PNP0A03");
    rres(ACPID_R_WINDOW, 0x10000000, 0x2eff0000, nullptr);
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.GPI0", 0xf);
    rkey("gpio-ctl");
    rres(ACPID_R_MMIO, 0x9030000, 0x1000, nullptr);
    rres(ACPID_R_IRQ, 39, 1, nullptr);
    hw_ns_add(&g, &rec);
    /* A child before its parent, naming devices that come later, and one
     * that never does. */
    rec_new("\\_SB_.I2C0.DEV0", 0xf);
    rkey("a");
    rres(ACPID_R_BUS, 0x50, 1, "\\_SB_.I2C0");
    rres(ACPID_R_GPIO, 7, 1, "\\_SB_.GPI0");
    rres(ACPID_R_IRQ, 3, 1, "\\_SB_.GPI0");
    rres(ACPID_R_GPIO, 2, 1, "\\_SB_.NONE");
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.I2C0", 0xf);
    rkey("i2c-ctl");
    rres(ACPID_R_MMIO, 0x9040000, 0x1000, nullptr);
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.OFF0", 0);
    rkey("a");
    hw_ns_add(&g, &rec);
    CHECK(g.broken == 0);

    /* Damaged records: each refused in part or whole, never read past. */
    broken = g.broken;
    rec_new("\\_SB_.BAD0", 0xf);
    rec.ad_Path = ACPID_STRINGS;            /* outside */
    hw_ns_add(&g, &rec);
    rec_new("_SB_.BAD1", 0xf);              /* not from the root */
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.TOOLONG", 0xf);
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.bad3", 0xf);
    hw_ns_add(&g, &rec);
    rec_new("\\_SB_.BAD4", 0xf);
    memset(rec.ad_Strings + rec_used, 'x', ACPID_STRINGS - rec_used);
    rec.ad_Keys[rec.ad_NKeys++] = (uint16_t)rec_used; /* no NUL before the end */
    rres(ACPID_R_MMIO, 0x9050000, 0, nullptr);       /* no size */
    rec.ad_Res[rec.ad_NRes++] = (struct AcpidRes){ .ar_Kind = 99 };
    rres(ACPID_R_BUS, 1, 1, nullptr);                /* a bus address on no bus */
    hw_ns_add(&g, &rec);
    CHECK(g.broken == broken + 4 + 4);
    broken = g.broken;
    rec_new("\\_SB_.BAD5", 0xf);
    rec.ad_NRes = 200;                               /* more than there are */
    rec.ad_NKeys = 200;
    hw_ns_add(&g, &rec);
    CHECK(g.broken > broken);
    CHECK(hw_find_path(&g, "/_SB_/BAD4", 10) != HW_NONE);
    CHECK(hw_find_path(&g, "/_SB_/TOOLONG", 13) == HW_NONE);
    hw_ns_done(&g);

    /* The twins moved, and kept what they had. */
    CHECK(hw_find_path(&g, "/_SB_/COM0", 10) == uart && g.console == uart);
    n = &g.nodes[uart];
    CHECK(n->nkeys == 2 && hw_streq(n->keys[0], "ARMH0011") && hw_streq(n->keys[1], "arm,pl011"));
    CHECK(n->nres == 1);                    /* its own: the SPCR's */
    CHECK(hw_find_moved(&g, "/SPCR/serial@9000000") == uart);
    CHECK(hw_find_path(&g, "/_SB_/DPU0", 10) == dpu && g.nodes[dpu].state == HW_S_STRUCTURE &&
          g.nodes[dpu].nres == 1 && g.nodes[dpu].res[0].base == 0x14000000);
    CHECK(hw_find_path(&g, "/_SB_/PCI0", 10) == pci && g.nodes[pci].nres == 2 &&
          g.nodes[pci].res[0].kind == HW_ECAM && g.nodes[pci].nkeys == 2);
    CHECK(g.nodes[pci].res[1].kind == HW_WINDOW && g.nodes[pci].res[1].base == 0x10000000 &&
          g.nodes[pci].res[1].cells[0] == HW_WIN_MEM);     /* its window, which the MCFG lacks */

    /* Interrupts on the GIC, and on a controller named by path. */
    gpio = hw_find_path(&g, "/_SB_/GPI0", 10);
    n = &g.nodes[gpio];
    CHECK(n->nres == 2 && n->res[1].kind == HW_IRQ && n->res[1].ctlr == gic &&
          n->res[1].base == 39);
    CHECK(n->nrefs == 1 && n->refs[0].kind == HW_REF_IRQ && n->refs[0].supplier == gic);
    dev0 = hw_find_path(&g, "/_SB_/I2C0/DEV0", 15);
    i2c = hw_find_path(&g, "/_SB_/I2C0", 10);
    CHECK(dev0 != HW_NONE && g.nodes[dev0].parent == i2c && g.nodes[i2c].nkeys == 1);
    n = &g.nodes[dev0];
    CHECK(n->nres == 2 && n->res[0].kind == HW_BUSADDR && n->res[0].base == 0x50);
    CHECK(n->res[1].kind == HW_IRQ && n->res[1].ctlr == gpio && n->res[1].base == HW_NOLINE &&
          n->res[1].cells[0] == 3);
    CHECK(n->nrefs == 4);                   /* the I2C0 isn't its parent yet: it had no keys */
    CHECK(n->refs[0].kind == HW_REF_PARENT && n->refs[0].supplier == i2c);
    CHECK(n->refs[1].kind == HW_REF_GPIO && n->refs[1].supplier == gpio && n->refs[1].cells[0] == 7);
    CHECK(n->refs[2].kind == HW_REF_IRQ && n->refs[2].supplier == gpio);
    CHECK(n->refs[3].target == HW_NONE && hw_streq(n->refs[3].path, "\\_SB_.NONE"));
    off = hw_find_path(&g, "/_SB_/OFF0", 10);
    CHECK(off != HW_NONE && !g.nodes[off].enabled);

    hw_match(&g, drivers, 3);
    hw_order_start(&g);
    hw_bind_all(&g);
    CHECK(g.nodes[off].state == HW_S_DISABLED);
    CHECK(strstr(line_for("/_SB_/I2C0/DEV0"), "\\_SB_.NONE, which names no device (gpio)"));
}

/* acpid's records from captured tables, damaged at random, merged into
 * the graph those tables give: never read past, whatever they say. */
static void test_ns_damage(const char *ns, const char *const *tables, unsigned ntables)
{
    uint32_t len, tlen[32];
    struct AcpidDevice *d = slurp(ns, &len), *copy;
    uint32_t nrec = len / (uint32_t)sizeof *d;
    void *tdata[32];
    struct hw_table t[32];

    for (unsigned i = 0; i < ntables && i < 32; i++)
        tdata[i] = slurp(tables[i], &tlen[i]);
    copy = malloc(len ? len : 1);
    for (unsigned round = 0; round < 300; round++) {
        memcpy(copy, d, len);
        for (unsigned k = 0; k < 1 + rnd() % 64 && len; k++)
            ((uint8_t *)copy)[rnd() % len] = (uint8_t)rnd();
        for (unsigned i = 0; i < ntables && i < 32; i++)
            t[i] = (struct hw_table){ tdata[i], tlen[i] };
        empty();
        if (!hw_from_acpi(&g, t, ntables))
            continue;
        for (uint32_t r = 0; r < nrec; r++)
            hw_ns_add(&g, &copy[r]);
        hw_ns_done(&g);
        hw_prepare(&g);
        hw_bind_all(&g);
        hw_report(&g, true, print, nullptr);
    }
    CHECK(nrec > 0);
    free(copy);
    free(d);
    for (unsigned i = 0; i < ntables && i < 32; i++)
        free(tdata[i]);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--namespace") == 0) {
        test_ns_damage(argv[2], (const char *const *)argv + 3, (unsigned)(argc - 3));
        printf("hwdb-test: %u of %u checks failed (acpid's records, damaged)\n", failures, checks);
        return failures ? 1 : 0;
    }
    test_order();
    test_match();
    test_quirks();
    test_leases();
    test_late();
    test_namespace();
    test_dma();
    if (argc >= 3)
        test_damage(argv[1], (const char *const *)argv + 2, (unsigned)(argc - 2));
    printf("hwdb-test: %u of %u checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
