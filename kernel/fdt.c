/*
 * kernel/fdt.c -- read-only flattened device tree access
 *
 * The blob is big-endian on every target, so every field goes through
 * get_be32.
 */
#include <vectra/endian.h>

#include "fdt.h"
#include "kprintf.h"
#include "kstring.h"

constexpr uint32_t FDT_MAGIC      = 0xd00dfeed;
constexpr uint32_t FDT_BEGIN_NODE = 1;
constexpr uint32_t FDT_END_NODE   = 2;
constexpr uint32_t FDT_PROP       = 3;
constexpr uint32_t FDT_NOP        = 4;
constexpr uint32_t FDT_END        = 9;

constexpr int MAX_DEPTH = 16;

static const uint8_t *blob;
static const uint8_t *structs;
static const char *strings;
static uint32_t struct_size;

/* Does node name 'name' ("memory@40000000") match path component 'comp'? */
static bool name_matches(const char *name, const char *comp, size_t len)
{
    bool has_unit = false;
    size_t i;

    for (i = 0; i < len; i++) {
        if (name[i] != comp[i])
            return false;
        has_unit |= comp[i] == '@';
    }
    return name[i] == '\0' || (name[i] == '@' && !has_unit);
}

static uint32_t token(uint32_t off)
{
    return off + 4 <= struct_size ? get_be32(structs + off) : FDT_END;
}

static uint32_t align4(uint32_t x)
{
    return (x + 3) & ~3u;
}

/* Offset of the first token after a node's BEGIN_NODE and name. */
static uint32_t node_body(int32_t node)
{
    return align4((uint32_t)node + 4 + (uint32_t)kstrlen(fdt_node_name(node)) + 1);
}

/* The next node in document order, adjusting *depth by the levels moved. */
static int32_t next_node(int32_t node, int *depth)
{
    uint32_t off;

    if (node < 0) {
        *depth = 0;
        return token(0) == FDT_BEGIN_NODE ? 0 : -1;
    }
    off = node_body(node);
    for (;;) {
        switch (token(off)) {
        case FDT_PROP:
            off = align4(off + 12 + token(off + 4));
            break;
        case FDT_NOP:
            off += 4;
            break;
        case FDT_BEGIN_NODE:
            (*depth)++;
            return (int32_t)off;
        case FDT_END_NODE:
            (*depth)--;
            off += 4;
            break;
        default:
            return -1;
        }
    }
}

bool fdt_init(const void *b)
{
    const uint8_t *p = b;

    if (!p || get_be32(p) != FDT_MAGIC || get_be32(p + 20) < 17)
        return false;
    blob = p;
    structs = p + get_be32(p + 8);
    strings = (const char *)(p + get_be32(p + 12));
    struct_size = get_be32(p + 36);
    return true;
}

const void *fdt_blob(void)
{
    return blob;
}

uint32_t fdt_size(void)
{
    return blob ? get_be32(blob + 4) : 0;
}

bool fdt_memreserve(uint32_t index, uint64_t *addr, uint64_t *size)
{
    const uint8_t *p;

    if (!blob)
        return false;
    p = blob + get_be32(blob + 16) + 16 * index;
    *addr = get_be64(p);
    *size = get_be64(p + 8);
    return *addr || *size;
}

const char *fdt_node_name(int32_t node)
{
    return (const char *)(structs + node + 4);
}

const void *fdt_getprop(int32_t node, const char *name, uint32_t *len)
{
    uint32_t off;

    if (!blob || node < 0)
        return nullptr;
    for (off = node_body(node);; ) {
        uint32_t tok = token(off);

        if (tok == FDT_NOP) {
            off += 4;
            continue;
        }
        if (tok != FDT_PROP)
            return nullptr;
        if (kstreq(strings + token(off + 8), name)) {
            if (len)
                *len = token(off + 4);
            return structs + off + 12;
        }
        off = align4(off + 12 + token(off + 4));
    }
}

uint32_t fdt_getprop_u32(int32_t node, const char *name, uint32_t fallback)
{
    uint32_t len;
    const uint8_t *p = fdt_getprop(node, name, &len);

    return p && len >= 4 ? get_be32(p) : fallback;
}

/* Walk 'len' bytes of absolute path from the root. */
static int32_t walk_path(const char *path, size_t len)
{
    const char *end = path + len;
    int32_t node;
    int depth;

    if (!blob || len == 0 || *path != '/')
        return -1;
    node = next_node(-1, &depth);
    while (node >= 0 && path < end && *path == '/') {
        const char *comp = ++path;
        size_t clen = 0;
        int want = depth + 1;

        while (comp + clen < end && comp[clen] != '/')
            clen++;
        if (clen == 0)
            break;
        path += clen;
        /* Search this node's children for the component. */
        do
            node = next_node(node, &depth);
        while (node >= 0 && depth >= want &&
               (depth != want || !name_matches(fdt_node_name(node), comp, clen)));
        if (node < 0 || depth < want)
            return -1;
    }
    return node;
}

int32_t fdt_path(const char *path)
{
    return walk_path(path, kstrlen(path));
}

int32_t fdt_path_or_alias(const char *path, size_t len)
{
    int32_t aliases;
    uint32_t off;

    if (len && *path == '/')
        return walk_path(path, len);
    /* An alias: a property of /aliases whose value is the full path. */
    aliases = fdt_path("/aliases");
    if (aliases < 0)
        return -1;
    for (off = node_body(aliases);; ) {
        uint32_t tok = token(off);
        const char *name;

        if (tok == FDT_NOP) {
            off += 4;
            continue;
        }
        if (tok != FDT_PROP)
            return -1;
        name = strings + token(off + 8);
        if (kstrlen(name) == len && memcmp(name, path, len) == 0) {
            const char *target = (const char *)(structs + off + 12);
            return fdt_path(target);
        }
        off = align4(off + 12 + token(off + 4));
    }
}

bool fdt_is_compatible(int32_t node, const char *compat)
{
    uint32_t len, off = 0;
    const char *list = fdt_getprop(node, "compatible", &len);

    if (!list)
        return false;
    while (off < len) {
        if (kstreq(list + off, compat))
            return true;
        off += (uint32_t)kstrlen(list + off) + 1;
    }
    return false;
}

int32_t fdt_find_compatible(int32_t after, const char *compat)
{
    int depth = 0;
    int32_t node = blob ? next_node(after, &depth) : -1;

    while (node >= 0 && !fdt_is_compatible(node, compat))
        node = next_node(node, &depth);
    return node;
}

int32_t fdt_find_name(int32_t after, const char *name)
{
    int depth = 0;
    int32_t node = blob ? next_node(after, &depth) : -1;

    while (node >= 0 && !name_matches(fdt_node_name(node), name, kstrlen(name)))
        node = next_node(node, &depth);
    return node;
}

int32_t fdt_child(int32_t parent, int32_t after)
{
    int depth = 0;
    bool past = after < 0;
    int32_t node = parent;

    if (!blob || parent < 0)
        return -1;
    while ((node = next_node(node, &depth)) >= 0 && depth > 0) {
        if (depth != 1)
            continue;
        if (past)
            return node;
        past = node == after;
    }
    return -1;
}

static int32_t parent_of(int32_t target)
{
    int32_t stack[MAX_DEPTH];
    int depth;
    int32_t node = next_node(-1, &depth);

    while (node >= 0 && node != target) {
        if (depth < MAX_DEPTH)
            stack[depth] = node;
        node = next_node(node, &depth);
    }
    return node >= 0 && depth > 0 && depth <= MAX_DEPTH ? stack[depth - 1] : -1;
}

static uint64_t read_cells(const uint8_t *p, uint32_t n)
{
    uint64_t v = 0;

    while (n--) {
        v = (v << 32) | get_be32(p);
        p += 4;
    }
    return v;
}

bool fdt_reg(int32_t node, uint32_t index, uint64_t *addr, uint64_t *size)
{
    int32_t parent = parent_of(node);
    uint32_t ac = fdt_getprop_u32(parent, "#address-cells", 2);
    uint32_t sc = fdt_getprop_u32(parent, "#size-cells", 1);
    uint32_t len, stride = (ac + sc) * 4;
    const uint8_t *p = fdt_getprop(node, "reg", &len);

    if (!p || ac > 2 || sc > 2 || stride == 0 || (index + 1) * stride > len)
        return false;
    p += index * stride;
    *addr = read_cells(p, ac);
    *size = read_cells(p + ac * 4, sc);
    return true;
}

static bool is_string_list(const uint8_t *p, uint32_t len)
{
    if (len == 0 || p[len - 1] != '\0' || p[0] == '\0')
        return false;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] == '\0') {
            if (i + 1 < len && p[i + 1] == '\0')
                return false;
        } else if (p[i] < 0x20 || p[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

static void print_value(const uint8_t *p, uint32_t len)
{
    uint32_t i;

    if (is_string_list(p, len)) {
        for (i = 0; i < len; i += (uint32_t)kstrlen((const char *)p + i) + 1)
            kprintf("%s\"%s\"", i ? ", " : "", (const char *)p + i);
    } else if (len % 4 == 0) {
        kputs("<");
        for (i = 0; i < len && i < 64; i += 4)
            kprintf("%s0x%x", i ? " " : "", get_be32(p + i));
        kputs(i < len ? " ...>" : ">");
    } else {
        kputs("[");
        for (i = 0; i < len && i < 32; i++)
            kprintf("%s%02x", i ? " " : "", p[i]);
        kputs(i < len ? " ...]" : "]");
    }
}

void fdt_dump_node(int32_t node, const char *const *only)
{
    uint32_t off;

    if (!blob || node < 0)
        return;
    kprintf("  %s\n", fdt_node_name(node));
    for (off = node_body(node);; ) {
        uint32_t tok = token(off), len;
        const char *name;
        const char *const *o;

        if (tok == FDT_NOP) {
            off += 4;
            continue;
        }
        if (tok != FDT_PROP)
            return;
        len = token(off + 4);
        name = strings + token(off + 8);
        for (o = only; o && *o && !kstreq(*o, name); o++)
            ;
        if (!only || *o) {
            kprintf("    %s", name);
            if (len) {
                kputs(" = ");
                print_value(structs + off + 12, len);
            }
            kputs("\n");
        }
        off = align4(off + 12 + len);
    }
}

void fdt_report(void)
{
    const char *model = fdt_getprop(fdt_path("/"), "model", nullptr);
    int32_t node = -1, cpus = fdt_path("/cpus");
    uint64_t addr, size;
    unsigned ncpu = 0;

    kprintf("vectra: device tree at %p, %u bytes, model \"%s\"\n",
            (const void *)blob, fdt_size(), model ? model : "?");
    while ((node = fdt_find_name(node, "memory")) >= 0)
        for (uint32_t i = 0; fdt_reg(node, i, &addr, &size); i++)
            kprintf("vectra: memory %p, %llu MiB\n", (void *)(uintptr_t)addr,
                    (unsigned long long)(size >> 20));
    for (uint32_t i = 0; fdt_memreserve(i, &addr, &size); i++)
        kprintf("vectra: reserved %p, %llu KiB\n", (void *)(uintptr_t)addr,
                (unsigned long long)(size >> 10));
    for (node = fdt_child(cpus, -1); node >= 0; node = fdt_child(cpus, node))
        if (name_matches(fdt_node_name(node), "cpu", 3))
            ncpu++;
    kprintf("vectra: %u CPUs in the device tree\n", ncpu);
}
