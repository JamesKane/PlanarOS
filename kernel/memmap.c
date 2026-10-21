/*
 * kernel/memmap.c -- the physical memory map, from the EFI memory map or
 * the device tree
 */
#include "acpi.h"
#include "arch.h"
#include "bootinfo.h"
#include "efi.h"
#include "fdt.h"
#include "kprintf.h"
#include "memmap.h"

constexpr unsigned MAX_RANGES = 64;
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

/* As add(), but extend the last range if this one follows it: the EFI
 * map splits RAM into many ranges by type. */
static void add_merged(struct range *list, unsigned *n, uint64_t base, uint64_t size)
{
    if (*n && list[*n - 1].base + list[*n - 1].size == base)
        list[*n - 1].size += size;
    else
        add(list, n, base, size);
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

uint64_t memmap_unmapped(void)
{
    return unmapped;
}

/* From the device tree's memory nodes. */
static void fdt_ram(void)
{
    int32_t node = -1;
    uint64_t base, size;

    while ((node = fdt_find_name(node, "memory")) >= 0) {
        for (uint32_t i = 0; fdt_reg(node, i, &base, &size); i++) {
            uint64_t lo = (base + ROUND - 1) & ~(ROUND - 1);
            uint64_t hi = (base + size) & ~(ROUND - 1);

            if (hi > lo)
                add(ram, &nram, lo, hi - lo);
        }
    }
}

/*
 * From the EFI memory map the stub kept. RAM is every write-back range
 * the firmware or the OS keeps something in; free is conventional and
 * boot-services memory, now that boot services are gone. The rest of
 * RAM is reserved: the runtime services' (never called, but not ours),
 * ACPI's, the loader's (the kernel, this map, the boot block), and a
 * framebuffer's made of RAM.
 */
static void efi_ram(const struct boot_info *bi)
{
    const uint8_t *map = phys_to_virt(bi->mmap);

    if (bi->mmap_desc_size < sizeof(struct efi_memory_descriptor))
        return;                 /* a broken map */
    for (uint64_t off = 0; off + sizeof(struct efi_memory_descriptor) <= bi->mmap_size;
         off += bi->mmap_desc_size) {
        const struct efi_memory_descriptor *d = (const void *)(map + off);
        uint64_t size = d->pages * EFI_PAGE_SIZE;

        if (!(d->attribute & EFI_MEMORY_WB))
            continue;
        switch (d->type) {
        case EFI_BOOT_SERVICES_CODE:
        case EFI_BOOT_SERVICES_DATA:
        case EFI_CONVENTIONAL_MEMORY:
            add_merged(ram, &nram, d->physical_start, size);
            break;
        case EFI_LOADER_CODE:
        case EFI_LOADER_DATA:
        case EFI_RUNTIME_SERVICES_CODE:
        case EFI_RUNTIME_SERVICES_DATA:
        case EFI_ACPI_RECLAIM_MEMORY:
        case EFI_ACPI_MEMORY_NVS:
        case EFI_PERSISTENT_MEMORY:
            add_merged(ram, &nram, d->physical_start, size);
            add_merged(reserved, &nreserved, d->physical_start, size);
            break;
        default:
            /* The GOP's framebuffer, when the firmware made it of RAM
             * (QEMU's ramfb): RAM still, kept from the allocator, so the
             * machine's RAM reads the same whether or not there's one. */
            if (bi->fb.base && d->physical_start <= bi->fb.base &&
                bi->fb.base + bi->fb.size <= d->physical_start + size) {
                add_merged(ram, &nram, d->physical_start, size);
                add_merged(reserved, &nreserved, d->physical_start, size);
            }
            break;
        }
    }
    /* Rounded inward once merged, as fdt_ram() rounds each range. */
    for (unsigned i = 0; i < nram; i++) {
        uint64_t lo = (ram[i].base + ROUND - 1) & ~(ROUND - 1);
        uint64_t hi = (ram[i].base + ram[i].size) & ~(ROUND - 1);

        if (hi > lo)
            ram[i] = (struct range){ lo, hi - lo };
        else
            ram[i--] = ram[--nram];
    }
}

bool memmap_init(void)
{
    const struct boot_info *bi = bootinfo();
    int32_t resv;
    uint64_t base, size;

    nram = nreserved = 0;
    if (bi && bi->mmap_size)
        efi_ram(bi);
    else
        fdt_ram();

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
    /* What an ACPI board's quirks say the map gets wrong (kernel/acpi.c). */
    for (unsigned i = 0; acpi_reserved(i, &base, &size); i++)
        add(reserved, &nreserved, base, size);
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
