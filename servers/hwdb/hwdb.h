/*
 * servers/hwdb/hwdb.h -- the device graph (docs §9.4 Layer 1, §11.5, §11.6)
 *
 * One graph of nodes, built from a source (the device tree, or ACPI's
 * static tables), changed by the board's quirks, matched against the
 * drivers' match tables, and then bound in dependency order: a node is
 * ready when every supplier it requires is bound, and when one binds only
 * its consumers are looked at again.
 *
 * Pure C with no library: it runs as the hwdb server and, for testing, in
 * a host program (servers/hwdb/host.c) on captured trees and tables. All
 * memory comes from one arena the caller gives it. The firmware wrote the
 * source, but the graph never trusts it: every offset and count is
 * checked, and what doesn't fit is dropped and counted (hw_graph.broken).
 */
#ifndef HWDB_H
#define HWDB_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t hw_id;

constexpr hw_id HW_NONE = UINT32_MAX;
constexpr uint64_t HW_NOLINE = UINT64_MAX;      /* an interrupt we can't number */
constexpr unsigned HW_CELLS = 4;                /* specifier cells kept */

/* --- Memory. --- */

struct hw_arena {
    uint8_t *base;
    size_t size, used;
};

/* n zeroed bytes, 8-aligned; nullptr when the arena is full. */
void *hw_alloc(struct hw_arena *a, size_t n);

/* --- Nodes. --- */

enum hw_res_kind : uint8_t {
    HW_MMIO,            /* base, size: physical */
    HW_IRQ,             /* ctlr, cells; base is the line, or HW_NOLINE */
    HW_BUSADDR,         /* base: an address on a bus that isn't memory (I2C, SPI) */
    HW_ECAM,            /* base, size; cells: segment, first bus, last bus */
    HW_WINDOW,          /* a host bridge's: base, size (the CPU's); cells: HW_WIN_*, bus address hi, lo */
    HW_MSIMAP,          /* a host bridge's: requester IDs base to base + size become
                           MSI device IDs from cells[0] (msi-map, the IORT) */
    HW_MSIX,            /* a PCI function's: its MSI-X table at base, size vectors */
};

/* HW_WINDOW's cells[0]. */
constexpr uint32_t HW_WIN_MEM = 1u << 0;        /* else I/O space */
constexpr uint32_t HW_WIN_64 = 1u << 1;         /* above 4 GiB on the bus */
constexpr uint32_t HW_WIN_PREFETCH = 1u << 2;

struct hw_res {
    enum hw_res_kind kind;
    uint8_t ncells;
    hw_id ctlr;
    uint64_t base, size;
    uint32_t cells[HW_CELLS];
};

/* What a reference asks of its supplier. */
enum hw_ref_kind : uint8_t {
    HW_REF_PARENT,      /* the bus the node sits on */
    HW_REF_IRQ,         /* its interrupt controller */
    HW_REF_CLOCK,
    HW_REF_RESET,
    HW_REF_POWER,       /* a power domain */
    HW_REF_SUPPLY,      /* a regulator */
    HW_REF_PHY,
    HW_REF_PINCTRL,
    HW_REF_GPIO,
    HW_REF_IOMMU,
    HW_REF_MSI,
    HW_REF_DMA,
    HW_REF_NVMEM,
    HW_REF_KINDS
};

struct hw_ref {
    enum hw_ref_kind kind;
    bool required;          /* else it orders, but is dropped if it can't be met */
    bool dropped;
    uint8_t ncells;
    hw_id target;           /* the node named, perhaps a part of a device */
    hw_id supplier;         /* the device that provides it; HW_NONE if none does */
    uint32_t handle;        /* the device tree's phandle, when it names none */
    const char *path;       /* acpid's: the device it names ("\_SB_.GPI4"), when none */
    const char *name;       /* the property, as "vmmc-supply" */
    uint32_t cells[HW_CELLS];
};

/* Where a node is: in the order the report gives them. */
enum hw_state : uint8_t {
    HW_S_STRUCTURE,     /* no keys: part of the description, not a device */
    HW_S_DISABLED,
    HW_S_KERNEL,        /* the kernel drives it */
    HW_S_FIRMWARE,      /* the firmware set it up and keeps it (a quirk, or M-mode's) */
    HW_S_NAMESPACE,     /* a static table names it; the rest comes from acpid */
    HW_S_NODRIVER,
    HW_S_WAITING,       /* matched; a supplier isn't bound yet */
    HW_S_READY,         /* matched, suppliers bound; its driver is being started */
    HW_S_BOUND,
    HW_S_DECLINED,      /* its driver looked and found nothing it drives (an empty slot) */
    HW_S_FAILED,        /* it can't be leased, or its driver failed too often: 'why' */
    HW_S_STATES
};

struct hw_node {
    hw_id id, parent;
    const char *name;               /* "sdh@d4280000"; the path is built from parents */
    const char **keys;              /* compatible, or _HID then _CIDs: most specific first */
    struct hw_res *res;
    struct hw_ref *refs;
    hw_id *consumers;               /* nodes with a reference to this one */
    uint16_t nkeys, nres, nrefs, nconsumers, maxconsumers;
    bool enabled;
    enum hw_state state;
    uint16_t driver;                /* index into the match tables; HW_NODRV */
    const char *why;                /* for KERNEL, FIRMWARE and FAILED: what says so */
    const char *pin, *exclude;      /* a quirk's: the driver it must or mustn't have */
    hw_id alias;                    /* a quirk's: the device this describes again; HW_NONE */
    hw_id with;                     /* a quirk's: a node leased with this one; HW_NONE */
    uint32_t pending;               /* references not yet met */
    uint32_t order;                 /* when it was bound, from 1 */
    uint32_t fdt_props;             /* the device tree: where its properties start */
    uint32_t phandle;
    uint16_t lease;                 /* its index in hw_graph.leases; HW_NOLEASE */
    uint16_t unit;                  /* its number among its driver's nodes */
    uint64_t dma_limit;             /* its DMA sees memory where the CPU does only below this
                                     * (dma-ranges, or a quirk); 0: everywhere */
    uint32_t call;                  /* a firmware call its driver may make (a quirk); 0: none */
    const char *power_set;          /* power's named set it needs before its lease (a quirk) */
    bool bare;                      /* hwdb's own, with no registers: leased nothing (QEMU's
                                     * power model); any other such node can't be leased */
};

constexpr uint16_t HW_NODRV = UINT16_MAX;
constexpr uint16_t HW_NOLEASE = UINT16_MAX;

/*
 * What one driver process is leased (lease.c): every device whose
 * registers share a page with another goes in the same lease, served by
 * one process. Each range becomes a kernel lease; the first carries the
 * lines. The sizes match the kernel's (planar/lease.h).
 */
constexpr unsigned HW_LEASE_RANGES = 4;
constexpr unsigned HW_LEASE_NODES = 32;
constexpr unsigned HW_LEASE_LINES = 32;

struct hw_range {
    uint64_t base, size;            /* whole pages */
};

struct hw_lease {
    uint16_t driver;
    uint8_t nranges, nlines;
    uint16_t nnodes;
    struct hw_range ranges[HW_LEASE_RANGES];
    hw_id nodes[HW_LEASE_NODES];
    uint32_t lines[HW_LEASE_LINES];
    uint64_t dma_limit;             /* the lowest of its nodes'; 0: none (LimitLeaseDma) */
    uint32_t call;                  /* a node's firmware call (SetLeaseCall); 0: none */
};

/* --- Match tables (§11.6). --- */

/* A driver's rank for a key: FreeBSD's scale. A node's own order of keys
 * comes first; the rank only settles which driver takes a key. */
enum hw_rank : uint8_t { HW_GENERIC = 1, HW_DEFAULT, HW_VENDOR, HW_SPECIFIC };

struct hw_match {
    const char *key;
    enum hw_rank rank;
};

/* Who a node is bound to. The kernel's and the firmware's entries say who
 * already drives it; the hwdb's are nodes it serves itself (buses whose
 * children it already knows, fixed clocks). */
enum hw_driver_kind : uint8_t { HW_DRV_PROCESS, HW_DRV_KERNEL, HW_DRV_FIRMWARE, HW_DRV_HWDB };

struct hw_driver {
    const char *name;
    enum hw_driver_kind kind;
    const struct hw_match *match;   /* ends with a nullptr key */
};

/* --- The graph. --- */

struct hw_graph {
    struct hw_arena *arena;
    struct hw_node *nodes;
    uint32_t count, cap;
    const char *board;              /* the root compatible, or "OEMID OEMTABLEID" */
    hw_id console;
    const struct hw_driver *drivers;
    unsigned ndrivers;

    /* The device tree source. */
    const uint8_t *fdt;
    uint32_t fdt_struct, fdt_struct_size, fdt_strings, fdt_strings_size;
    uint32_t *phandles;             /* sorted pairs: phandle, node */
    uint32_t nphandles;

    /* The namespace (namespace.c): the first node acpid's devices added,
     * and the static nodes it moved, by the paths they had (quirks name
     * them so: hw_find_moved). */
    uint32_t ns_first;
    struct hw_move { const char *from; hw_id node; } *moves;
    uint32_t nmoves, maxmoves;

    /* Leases (lease.c), with room for HW_LATE_LEASES more (hw_add_late). */
    struct hw_lease *leases;
    uint32_t nleases, maxleases;

    /* Ordering. */
    hw_id *queue;                   /* ready nodes, in the order they became ready */
    uint32_t qhead, qtail, bound;
    uint32_t examined;              /* times a waiting node was looked at again */

    unsigned broken;                /* parts of the source refused, or dropped for space */
    bool full;                      /* the arena ran out */
};

void hw_graph_init(struct hw_graph *g, struct hw_arena *a, uint32_t cap);
hw_id hw_node_new(struct hw_graph *g, hw_id parent, const char *name);
struct hw_res *hw_res_add(struct hw_graph *g, hw_id node);
struct hw_ref *hw_ref_add(struct hw_graph *g, hw_id node, enum hw_ref_kind kind,
                          const char *name, bool required);
bool hw_key_add(struct hw_graph *g, hw_id node, const char *key);
bool hw_has_key(const struct hw_node *n, const char *key);
const char *hw_ref_kind_name(enum hw_ref_kind kind);

/* The device a reference to 'target' depends on: 'target' itself if it
 * has keys, else its nearest ancestor that has; never the root, which
 * describes the board and isn't a device. */
hw_id hw_device_of(const struct hw_graph *g, hw_id target);

/* Sources. Each builds the graph from nothing; false if the source is
 * unusable. The device tree's blob must stay where it is: the graph
 * points into it. */
bool hw_from_fdt(struct hw_graph *g, const void *blob, uint32_t size);
const void *hw_fdt_prop(const struct hw_graph *g, hw_id node, const char *name, uint32_t *len);

/* ACPI's static tables (§9.4: what needs no AML): the MADT's GIC, the
 * MCFG, the IORT, the SPCR or DBG2, the GTDT's watchdogs. */
struct hw_table {
    const void *data;
    uint32_t len;
};
bool hw_from_acpi(struct hw_graph *g, const struct hw_table *tables, unsigned n);

/*
 * The namespace (namespace.c, docs/v4-plan.md step 4): acpid's devices,
 * added to a graph hw_from_acpi built, one at a time as they come, then
 * hw_ns_done once all have. Each goes where its path says ("\_SB_.COM0"
 * is /_SB_/COM0). A device a static table already has (an IORT named
 * component, the MCFG's host bridge, a serial port at the same address)
 * isn't added again: the static node moves to the path and takes the
 * device's keys. hw_ns_done resolves the paths resources name.
 */
struct AcpidDevice;
void hw_ns_add(struct hw_graph *g, const struct AcpidDevice *d);
void hw_ns_done(struct hw_graph *g);

/*
 * Board quirks (§9.4 Layer 2, D5 (a)): C tables, keyed by the board (the
 * root compatible, or the ACPI OEM ID and OEM table ID) and naming nodes
 * by path. They change the graph, never a match table.
 */
enum hw_quirk_kind : uint8_t {
    HW_Q_DISABLE,       /* the node isn't there, whatever the firmware says */
    HW_Q_FIRMWARE,      /* the firmware set it up and leaves it so: its consumers needn't wait */
    HW_Q_CONSOLE,       /* the kernel's console, where the firmware doesn't say */
    HW_Q_PIN,           /* bind to this driver only */
    HW_Q_EXCLUDE,       /* never to this driver */
    HW_Q_ALIAS,         /* the device at 'arg' described again: its registers are that one's */
    HW_Q_DMA_LIMIT,     /* its DMA sees memory where the CPU does only below 'arg' */
    HW_Q_REGS,          /* of its registers, only the blocks at the addresses in 'arg' are its */
    HW_Q_MMIO,          /* registers the firmware doesn't list: 'arg' is "base size" */
    HW_Q_CALL,          /* its driver may make the firmware call 'arg' (SetLeaseCall) */
    HW_Q_IRQ,           /* a line the firmware doesn't list: 'arg' is "line controller-path" */
    HW_Q_SCANOUT,       /* it scans out the firmware's framebuffer: leased with it, to one process */
    HW_Q_NOIRQ,         /* its lines through the controller at 'arg' ("[line] path") aren't used */
    HW_Q_POWER,         /* it needs power's named set 'arg', voted for before its lease */
    HW_Q_NODE,          /* a device the firmware doesn't list, at the path, with compatible 'arg' */
};

struct hw_quirk {
    const char *board;
    enum hw_quirk_kind kind;
    const char *path;
    const char *arg;    /* the driver, for PIN and EXCLUDE; numbers, in hex, for
                         * DMA_LIMIT, REGS, MMIO and CALL; the compatible for NODE;
                         * else why */
};

extern const struct hw_quirk hw_quirk_table[];
extern const unsigned hw_nquirks;

/* Apply the table's quirks for g's board; the count applied. A quirk
 * naming a node the graph hasn't got is counted in g->broken. */
unsigned hw_quirks(struct hw_graph *g, const struct hw_quirk *q, unsigned n);
/* (An alias quirk whose either node is missing just doesn't apply: the
 * namespace's devices aren't there when acpid fails.) */

/* The node at 'path' ("/soc/uart@d4017000"), or HW_NONE. */
hw_id hw_find_path(const struct hw_graph *g, const char *path, size_t len);
/* The node a static table put at 'path' and acpid's namespace moved, or HW_NONE. */
hw_id hw_find_moved(const struct hw_graph *g, const char *path);
hw_id hw_child(const struct hw_graph *g, hw_id parent, const char *name, size_t len);

/* Matching (match.c): each enabled device gets the best driver for its
 * most specific key that any driver takes. */
void hw_match(struct hw_graph *g, const struct hw_driver *drivers, unsigned n);
void hw_match_node(struct hw_graph *g, hw_id id);     /* one more, with the same drivers */

/* Group the process drivers' nodes into leases by the pages they share,
 * after hw_match; a node that can't be leased fails, saying why. */
void hw_leases(struct hw_graph *g);

/*
 * A node a bus driver published once the graph was running (the pci
 * server's functions, step 5): matched, numbered among its driver's
 * units, leased on its own, and queued once its references are met. Its
 * pages mustn't touch another device's: if they do, it fails, saying
 * which. Leases past HW_LATE_LEASES fail it too.
 */
constexpr unsigned HW_LATE_LEASES = 64;
void hw_add_late(struct hw_graph *g, hw_id id);
void hw_lease_late(struct hw_graph *g, hw_id id);     /* lease.c's half */

/* The drivers hwdb knows (drivers.c). */
extern const struct hw_driver hw_drivers[];
extern const unsigned hw_ndrivers;

/* Two drivers that take the same key at the same rank: each pair is
 * reported, and the count returned. */
typedef void hw_print(void *ctx, const char *line);
unsigned hw_match_ties(const struct hw_driver *drivers, unsigned n, hw_print *out, void *ctx);

/*
 * Ordering (order.c). hw_order_start finds what's ready; hw_next hands
 * out the next ready node (HW_NONE when none is); the server starts its
 * driver and calls hw_bound when it has bound, which looks again at that
 * node's consumers only, or hw_unbind when it can't (declined, failed),
 * which leaves them waiting. When nothing is ready and no driver is
 * still starting, hw_unstick drops the optional references that can't
 * be met (to a supplier with no driver, or in a cycle); true if that
 * made a node ready. hw_bind_all is the host's: every driver binds at
 * once.
 */
void hw_order_start(struct hw_graph *g);
hw_id hw_next(struct hw_graph *g);
bool hw_unstick(struct hw_graph *g);
void hw_bound(struct hw_graph *g, hw_id node);
void hw_unbind(struct hw_graph *g, hw_id node, enum hw_state state, const char *why);
void hw_bind_all(struct hw_graph *g);

/* hwdb's way from a firmware description to an ordered graph: the
 * board's quirks, matching, the units and leases, and what's ready. */
void hw_prepare(struct hw_graph *g);

/* Each device, with its state, then (if 'detail') its resources and
 * what it needs; and a count of each state. */
void hw_report(const struct hw_graph *g, bool detail, hw_print *out, void *ctx);

/* --- Small helpers (graph.c), there being no library. --- */

size_t hw_strlen(const char *s);
bool hw_streq(const char *a, const char *b);
bool hw_strneq(const char *a, const char *b, size_t n);   /* the first n bytes, and b ends there */
const char *hw_strdup(struct hw_graph *g, const char *s, size_t n);

/* A line of text, built a piece at a time and cut short if too long. */
struct hw_line {
    char text[480];
    unsigned len;
};
void hw_put(struct hw_line *l, const char *s);
void hw_put_hex(struct hw_line *l, uint64_t v);
void hw_put_dec(struct hw_line *l, uint64_t v);
void hw_put_path(struct hw_line *l, const struct hw_graph *g, hw_id node);

#endif /* HWDB_H */
