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
#include "kprintf.h"
#include "kstring.h"
#include "memmap.h"
#include "pmm.h"
#include "spinlock.h"

static atomic_uint bad_frees;
static atomic_uint_fast64_t fail_in;    /* tests: allocations left before one fails */
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
    uint64_t hi = (end + PAGE_SIZE - 1) >> PAGE_SHIFT;

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

    if (!bitmap || !size || !span_pages(start, start + size, &first, &last))
        return;
    unsigned long was = spin_lock_irqsave(&lock);
    set_range(first, last, true);
    spin_unlock_irqrestore(&lock, was);
}

bool pmm_init(void)
{
    uint64_t base, size, lo = ~(uint64_t)0, hi = 0, kstart, kend, bytes, first, last;

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
     * taken back. The bitmap goes in the pages after the kernel. */
    bitmap = phys_to_virt((kend + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(bitmap, 0xff, nwords * 8);
    nfree = 0;
    for (unsigned i = 0; memmap_ram(i, &base, &size); i++)
        if (span_pages(base, base + size, &first, &last))
            set_range(first, last, false);
    total = nfree;

    pmm_reserve(kstart, virt_to_phys(bitmap) + bytes - kstart);
    for (unsigned i = 0; memmap_reserved(i, &base, &size); i++)
        pmm_reserve(base, size);
    return true;
}

/* A test asked for this allocation to fail. */
static bool injected_failure(void)
{
    uint64_t n = atomic_load_explicit(&fail_in, memory_order_relaxed);

    return n && atomic_fetch_sub(&fail_in, 1) == 1;
}

void pmm_fail_after(uint64_t n)
{
    atomic_store(&fail_in, n ? n + 1 : 0);
}

uint64_t pmm_alloc(void)
{
    uint64_t page = 0;

    if (injected_failure())
        return 0;
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
    uint64_t start = 0, len = 0, page = 0, step = align / PAGE_SIZE;

    if (n == 0 || step == 0 || (step & (step - 1)))
        return 0;
    unsigned long was = spin_lock_irqsave(&lock);
    for (uint64_t i = 0; nfree >= n && i < npages; i++) {
        if (i % 64 == 0 && bitmap[i / 64] == ~0ULL) {
            i += 63;                /* a word of taken pages at a time */
            len = 0;
            continue;
        }
        if (taken(i)) {
            len = 0;
            continue;
        }
        if (len == 0 && (base_pfn + i) % step)
            continue;               /* a run may only start aligned */
        if (len++ == 0)
            start = i;
        if (len == n) {
            set_range(start, start + n, true);
            page = (base_pfn + start) << PAGE_SHIFT;
            break;
        }
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
