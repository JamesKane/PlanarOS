/*
 * kernel/library.c -- the shared library region (V6 step 4, docs §3.4)
 *
 * At boot every library image in the bundle (planar/libimage.h) is
 * copied into one Area, a page boundary apart, and relocated there once.
 * Then each alternative is applied: a table entry repointed to a
 * function that needs what this CPU has (exec's CopyMem by the vector
 * unit). Last, exec.library's directory gets every library's base, for
 * OpenLibrary. Every process then maps the whole Area read and execute,
 * global (MAP_GLOBAL): one copy of each library's pages, and in the TLB,
 * whichever process runs. Nothing in it is written again.
 */
#include <planar/endian.h>
#include <planar/libimage.h>

#include "arch.h"
#include "area.h"
#include "kprintf.h"
#include "kstring.h"
#include "pmm.h"
#include "userimg.h"

static struct area *region;
static uint64_t iexec;
static unsigned count;
static bool vector;

struct rela {
    uint64_t offset, info;
    int64_t addend;
};

/* The library images in the bundle, by their magic. */
static bool is_library(const struct user_image *img)
{
    return img->end - img->start >= (long)sizeof(struct LibraryHeader) &&
           get_le32(img->start) == LIBIMAGE_MAGIC;
}

/* Where the kernel reaches region address 'va' of 'n' bytes, or nullptr
 * if it isn't inside [lo, hi). */
static void *at(uint64_t va, uint64_t n, uint64_t lo, uint64_t hi)
{
    if (va < lo || n > hi - lo || va - lo > hi - lo - n)
        return nullptr;
    return (uint8_t *)phys_to_virt(area_pa(region, 0)) + (va - area_base(region));
}

/* Relocate the image at region offset 'off', and apply its LibraryImage:
 * its base, or 0 if it's malformed. */
static uint64_t load(const struct user_image *img, uint64_t off, uint64_t has)
{
    uint64_t lo = area_base(region) + off, size = get_le64(img->start + 8);
    uint64_t rs = get_le64(img->start + 16), re = get_le64(img->start + 24);
    uint64_t hi = lo + size;
    struct LibraryImage *li;

    if (get_le32(img->start + 4) != LIBIMAGE_FORMAT || size < (uint64_t)(img->end - img->start) ||
        rs > re || re > size || (re - rs) % sizeof(struct rela))
        return 0;
    for (uint64_t r = rs; r < re; r += sizeof(struct rela)) {
        struct rela *e = at(lo + r, sizeof *e, lo, hi);
        uint64_t *slot;

        if ((uint32_t)e->info == 0)
            continue;
        if ((uint32_t)e->info != ARCH_R_RELATIVE || !(slot = at(lo + e->offset, 8, lo, hi)))
            return 0;
        *slot = lo + (uint64_t)e->addend;
    }
    if (!(li = at(lo + get_le64(img->start + 32), sizeof *li, lo, hi)) ||
        !at((uint64_t)li->li_Library, sizeof(struct Library), lo, hi))
        return 0;
    for (uint64_t i = 0; i < li->li_NumAlts; i++) {
        struct LibraryAlt *a = at((uint64_t)li->li_Alts + i * sizeof *a, sizeof *a, lo, hi);
        uint64_t *slot;

        if (!a || !(slot = at((uint64_t)a->la_Slot, 8, lo, hi)) ||
            !at((uint64_t)a->la_Function, 4, lo, hi))
            return 0;
        if ((a->la_Needs & ~has) == 0) {
            *slot = (uint64_t)a->la_Function;
            vector |= (a->la_Needs & LIBF_VECTOR) != 0;
        }
    }
    return (uint64_t)li->li_Library;
}

/* exec.library's directory: every base, in the bundle's order. */
static bool fill_directory(const uint64_t *bases, const uint64_t *images, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint64_t lo = images[i], hi = area_base(region) + area_size(region);
        struct LibraryImage *li;
        struct Library *lib = at(bases[i], sizeof *lib, lo, hi);
        uint64_t *slots;

        if (!lib || !kstreq(at((uint64_t)lib->lib_Name, 1, lo, hi), "exec.library"))
            continue;
        li = at(lo + get_le64((uint8_t *)at(lo, 40, lo, hi) + 32), sizeof *li, lo, hi);
        if (!li || li->li_DirectorySlots < n ||
            !(slots = at((uint64_t)li->li_Directory, n * 8, lo, hi)))
            return false;
        for (unsigned j = 0; j < n; j++)
            slots[j] = bases[j];
        /* Its main interface: the first. */
        iexec = *(uint64_t *)at((uint64_t)lib->lib_Interfaces, 8, lo, hi);
        return true;
    }
    return false;
}

bool library_init(void)
{
    constexpr unsigned MAX = 16;
    uint64_t pages = 0, off = 0, bases[MAX], images[MAX];
    const struct user_image *img;
    unsigned n = 0;

    for (img = user_images; img->name; img++)
        if (is_library(img))
            pages += (get_le64(img->start + 8) + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!pages)
        return true;
    if (!(region = area_new(pages, AREAF_EXEC, false))) {
        kprintf("library: no room for the region (%llu pages)\n", (unsigned long long)pages);
        return false;
    }
    for (img = user_images; img->name; img++) {
        if (!is_library(img))
            continue;
        area_copy_in(region, off, img->start, (uint64_t)(img->end - img->start));
        if (n == MAX || !(bases[n] = load(img, off, arch_has_vector() ? LIBF_VECTOR : 0))) {
            kprintf("library: %s is malformed\n", img->name);
            goto fail;
        }
        images[n++] = area_base(region) + off;
        off += (get_le64(img->start + 8) + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    }
    if (!fill_directory(bases, images, n)) {
        kprintf("library: no exec.library, or its directory is too small\n");
        goto fail;
    }
    arch_sync_icache(phys_to_virt(area_pa(region, 0)), area_size(region));
    area_make_library(region);
    count = n;
    return true;
fail:
    area_destroy(region);
    region = nullptr;
    iexec = 0;
    return false;
}

bool library_map(struct process *p)
{
    return !region || area_map(region, p, AREAR_READ | AREAR_EXEC, area_size(region));
}

uint64_t library_iexec(void)
{
    return iexec;
}

uint64_t library_pages(void)
{
    return region ? area_size(region) / PAGE_SIZE : 0;
}

unsigned library_count(void)
{
    return count;
}

bool library_vector(void)
{
    return vector;
}
