/*
 * kernel/memmap.c -- the physical memory map, from the device tree
 */
#include "arch.h"
#include "fdt.h"
#include "kprintf.h"
#include "memmap.h"

constexpr unsigned MAX_RANGES = 32;
constexpr uint64_t ROUND = 1ULL << 21;

struct range {
    uint64_t base, size;
};

static struct range ram[MAX_RANGES], reserved[MAX_RANGES];
static unsigned nram, nreserved;

static void add(struct range *list, unsigned *n, uint64_t base, uint64_t size)
{
    if (size == 0)
        return;
    if (*n == MAX_RANGES) {
        kprintf("planar: memory map full; dropped %p+%llx\n", (void *)base,
                (unsigned long long)size);
        return;
    }
    list[(*n)++] = (struct range){ base, size };
}

static uint64_t unmapped;           /* RAM bytes past the direct map, dropped */

/* Keep RAM below 'top' only (the direct map's end). */
static void clip(uint64_t top)
{
    for (unsigned i = 0; i < nram; i++) {
        if (ram[i].base >= top) {
            unmapped += ram[i].size;
            ram[i--] = ram[--nram];
        } else if (ram[i].base + ram[i].size > top) {
            unmapped += ram[i].base + ram[i].size - top;
            ram[i].size = top - ram[i].base;
        }
    }
}

void memmap_clip(uint64_t top)
{
    clip(top);
}

bool memmap_init(void)
{
    int32_t node = -1, resv;
    uint64_t base, size;

    nram = nreserved = 0;
    while ((node = fdt_find_name(node, "memory")) >= 0) {
        for (uint32_t i = 0; fdt_reg(node, i, &base, &size); i++) {
            uint64_t lo = (base + ROUND - 1) & ~(ROUND - 1);
            uint64_t hi = (base + size) & ~(ROUND - 1);

            if (hi > lo)
                add(ram, &nram, lo, hi - lo);
        }
    }

    /* The kernel reaches RAM only through the direct map (review M13). */
    clip((uint64_t)ARCH_DIRECT_MAP_GIB << 30);

    for (uint32_t i = 0; fdt_memreserve(i, &base, &size); i++)
        add(reserved, &nreserved, base, size);
    if ((resv = fdt_path("/reserved-memory")) >= 0)
        for (int32_t c = fdt_child(resv, -1); c >= 0; c = fdt_child(resv, c))
            for (uint32_t i = 0; fdt_reg(c, i, &base, &size); i++)
                add(reserved, &nreserved, base, size);
    /* The device tree itself, which the kernel keeps reading. */
    add(reserved, &nreserved, virt_to_phys(fdt_blob()), fdt_size());
    return nram > 0;
}

bool memmap_ram(unsigned index, uint64_t *base, uint64_t *size)
{
    if (index >= nram)
        return false;
    *base = ram[index].base;
    *size = ram[index].size;
    return true;
}

bool memmap_reserved(unsigned index, uint64_t *base, uint64_t *size)
{
    if (index >= nreserved)
        return false;
    *base = reserved[index].base;
    *size = reserved[index].size;
    return true;
}

void memmap_report(void)
{
    if (unmapped)
        kprintf("planar: %llu MiB of RAM past the direct map, unused\n",
                (unsigned long long)(unmapped >> 20));
    for (unsigned i = 0; i < nram; i++)
        kprintf("planar: RAM      %p-%p, %llu MiB\n", (void *)ram[i].base,
                (void *)(ram[i].base + ram[i].size - 1),
                (unsigned long long)(ram[i].size >> 20));
    for (unsigned i = 0; i < nreserved; i++)
        kprintf("planar: reserved %p-%p, %llu KiB\n", (void *)reserved[i].base,
                (void *)(reserved[i].base + reserved[i].size - 1),
                (unsigned long long)(reserved[i].size >> 10));
}
