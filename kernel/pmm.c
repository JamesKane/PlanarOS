/*
 * kernel/pmm.c -- physical page allocator
 *
 * One bit per page, set when the page is in use or isn't memory at all,
 * over the span from the lowest to the highest RAM address. The bitmap
 * lives just after the kernel image. Allocation is next-fit, a 64-bit word
 * at a time, under one lock.
 */
#include <stdatomic.h>

#include "arch.h"
#include "fdt.h"
#include "kprintf.h"
#include "kstring.h"
#include "memmap.h"
#include "pmm.h"
#include "spinlock.h"

static atomic_uint bad_frees;
static uint64_t *bitmap;            /* bit n: page base_pfn + n is taken */
static uint64_t base_pfn, npages;   /* the span the bitmap covers */
static uint64_t nwords, next_word;
static uint64_t total, nfree;
static spinlock_t lock;

static bool taken(uint64_t n)
{
    return (bitmap[n / 64] >> (n % 64)) & 1;
}

static void set_range(uint64_t first, uint64_t last, bool used)
{
    for (uint64_t n = first; n < last; n++) {
        if (used && !taken(n)) {
            bitmap[n / 64] |= 1ULL << (n % 64);
            nfree--;
        } else if (!used && taken(n)) {
            bitmap[n / 64] &= ~(1ULL << (n % 64));
            nfree++;
        }
    }
}

/* Clip [start, end) to the span, in page numbers relative to base_pfn. */
static bool span_pages(uint64_t start, uint64_t end, uint64_t *first, uint64_t *last)
{
    uint64_t lo = start >> PAGE_SHIFT;
    uint64_t hi = (end >> PAGE_SHIFT) + ((end & (PAGE_SIZE - 1)) != 0);   /* no wrap at 2^64 */

    if (lo < base_pfn)
        lo = base_pfn;
    if (hi > base_pfn + npages)
        hi = base_pfn + npages;
    if (lo >= hi)
        return false;
    *first = lo - base_pfn;
    *last = hi - base_pfn;
    return true;
}

void pmm_reserve(uint64_t start, uint64_t size)
{
    uint64_t first, last;

    /* A range running past 2^64 is reserved to the top, not dropped (§10.8). */
    if (!bitmap || !size || !span_pages(start, size > ~0ULL - start ? ~0ULL : start + size,
                                        &first, &last))
        return;
    unsigned long was = spin_lock_irqsave(&lock);
    set_range(first, last, true);
    spin_unlock_irqrestore(&lock, was);
}

static bool overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}

uint64_t pmm_place(uint64_t start, uint64_t bytes, uint64_t avoid, uint64_t avoid_size)
{
    uint64_t at = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), base, size;

    for (unsigned rounds = 0; rounds < 64; rounds++) {
        uint64_t next = at;
        bool in_ram = false;

        /* Past anything kept, then into RAM. */
        if (overlaps(at, bytes, avoid, avoid_size))
            next = avoid + avoid_size;
        for (unsigned i = 0; memmap_reserved(i, &base, &size); i++)
            if (overlaps(next, bytes, base, size))
                next = base + size;
        next = (next + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (next != at) {
            at = next;
            continue;
        }
        for (unsigned i = 0; memmap_ram(i, &base, &size) && !in_ram; i++)
            in_ram = at >= base && at + bytes <= base + size;
        if (in_ram)
            return at;
        /* The next RAM range up. */
        next = ~0ULL;
        for (unsigned i = 0; memmap_ram(i, &base, &size); i++)
            if (base > at && base < next)
                next = base;
        if (next == ~0ULL)
            return 0;
        at = next;
    }
    return 0;
}

bool pmm_init(void)
{
    uint64_t base, size, lo = ~(uint64_t)0, hi = 0, kstart, kend, bytes, first, last, at;
    uint64_t fdt_pa, fdt_bytes;

    arch_kernel_extent(&kstart, &kend);
    for (unsigned i = 0; memmap_ram(i, &base, &size); i++) {
        if (base < lo)
            lo = base;
        if (base + size > hi)
            hi = base + size;
    }
    if (hi <= lo || kend <= kstart)
        return false;

    base_pfn = lo >> PAGE_SHIFT;
    npages = (hi >> PAGE_SHIFT) - base_pfn;
    nwords = (npages + 63) / 64;
    bytes = (nwords * 8 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Everything starts taken; then RAM is freed and the reservations
     * taken back. The bitmap goes after the kernel, clear of what the
     * firmware keeps and of the device tree (review M15). */
    fdt_pa = fdt_blob() ? virt_to_phys(fdt_blob()) : 0;
    fdt_bytes = fdt_size();
    if (!(at = pmm_place(kend, bytes, fdt_pa, fdt_bytes)))
        return false;
    bitmap = phys_to_virt(at);
    memset(bitmap, 0xff, nwords * 8);
    nfree = 0;
    for (unsigned i = 0; memmap_ram(i, &base, &size); i++)
        if (span_pages(base, base + size, &first, &last))
            set_range(first, last, false);
    total = nfree;

    pmm_reserve(kstart, kend - kstart);
    pmm_reserve(at, bytes);
    pmm_reserve(fdt_pa, fdt_bytes);         /* read again later; firmware may not list it */
    for (unsigned i = 0; memmap_reserved(i, &base, &size); i++)
        pmm_reserve(base, size);
    return true;
}

uint64_t pmm_alloc(void)
{
    uint64_t page = 0;

    unsigned long was = spin_lock_irqsave(&lock);

    for (uint64_t w = 0; nfree && w < nwords; w++) {
        uint64_t i = (next_word + w) % nwords, n;

        if (bitmap[i] == ~(uint64_t)0)
            continue;
        n = i * 64 + (uint64_t)__builtin_ctzll(~bitmap[i]);
        if (n >= npages)
            continue;
        bitmap[i] |= 1ULL << (n % 64);
        nfree--;
        next_word = i;
        page = (base_pfn + n) << PAGE_SHIFT;
        break;
    }
    spin_unlock_irqrestore(&lock, was);
    if (page)
        memset(phys_to_virt(page), 0, PAGE_SIZE);
    return page;
}

uint64_t pmm_alloc_contig(size_t n)
{
    return n == 1 ? pmm_alloc() : pmm_alloc_contig_aligned(n, PAGE_SIZE);
}

uint64_t pmm_alloc_contig_aligned(size_t n, uint64_t align)
{
    return pmm_alloc_contig_below(n, align, ~0ULL);
}

static uint64_t contig_steps;      /* the scan's steps, for the selftest */

uint64_t pmm_contig_steps(void)
{
    return contig_steps;
}

uint64_t pmm_span_pages(void)
{
    return npages;
}

/*
 * First fit, a run of the bitmap at a time: a taken run or a free one is
 * one step, found with ctz, not one step a page, so a fragmented span
 * costs its runs, not its pages, with the lock held and interrupts off
 * (§10.8). A run may only start aligned.
 */
uint64_t pmm_alloc_contig_below(size_t n, uint64_t align, uint64_t limit)
{
    uint64_t start = 0, len = 0, page = 0, step = align / PAGE_SIZE;
    uint64_t end = npages, i = 0;

    if (n == 0 || step == 0 || (step & (step - 1)))
        return 0;
    if (limit >> PAGE_SHIFT < base_pfn)
        return 0;
    if ((limit >> PAGE_SHIFT) - base_pfn < end)
        end = (limit >> PAGE_SHIFT) - base_pfn;     /* pages wholly below 'limit' */
    unsigned long was = spin_lock_irqsave(&lock);
    while (nfree >= n && i < end) {
        uint64_t w = bitmap[i / 64] >> (i % 64), left = 64 - i % 64, run;

        contig_steps++;
        if (w & 1) {                /* taken, to the first free bit */
            run = ~w ? (uint64_t)__builtin_ctzll(~w) : 64;
            i += run < left ? run : left;
            len = 0;
            continue;
        }
        run = w ? (uint64_t)__builtin_ctzll(w) : 64;
        if (run > left)
            run = left;
        if (run > end - i)
            run = end - i;
        if (len == 0) {             /* start at the first aligned page in it */
            uint64_t skip = (step - (base_pfn + i) % step) % step;

            if (skip >= run) {
                i += run;
                continue;
            }
            i += skip;
            run -= skip;
            start = i;
        }
        if (len + run >= n) {
            set_range(start, start + n, true);
            page = (base_pfn + start) << PAGE_SHIFT;
            break;
        }
        len += run;
        i += run;
    }
    spin_unlock_irqrestore(&lock, was);
    if (page)
        memset(phys_to_virt(page), 0, n * PAGE_SIZE);
    return page;
}

void pmm_free(uint64_t page)
{
    uint64_t n = (page >> PAGE_SHIFT) - base_pfn;
    unsigned long was = spin_lock_irqsave(&lock);

    if (!bitmap || n >= npages || !taken(n)) {
        spin_unlock_irqrestore(&lock, was);
        kprintf("planar: pmm_free of a page not allocated: %p (from %p)\n", (void *)page,
                __builtin_return_address(0));
        atomic_fetch_add(&bad_frees, 1);
        return;
    }
    bitmap[n / 64] &= ~(1ULL << (n % 64));
    nfree++;
    spin_unlock_irqrestore(&lock, was);
}

unsigned pmm_bad_frees(void)
{
    return atomic_load(&bad_frees);
}

uint64_t pmm_total(void)
{
    return total;
}

uint64_t pmm_free_pages(void)
{
    return nfree;
}

uint64_t pmm_largest_free_run(void)
{
    uint64_t best = 0, run = 0;
    unsigned long was = spin_lock_irqsave(&lock);

    for (uint64_t i = 0; i < npages; i++) {
        run = taken(i) ? 0 : run + 1;
        if (run > best)
            best = run;
    }
    spin_unlock_irqrestore(&lock, was);
    return best;
}
