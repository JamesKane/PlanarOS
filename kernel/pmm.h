/*
 * kernel/pmm.h -- physical page allocator
 *
 * Hands out 4 KiB physical pages. Every page is committed when allocated
 * (P5): an allocation succeeds now or returns 0 now. Safe on every CPU.
 * Zones (docs §3.4: stacks, slabs, large Areas, DMA32) come in V1 step 5.
 * From omega's kernel/pmm.c.
 */
#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stddef.h>
#include <stdint.h>

constexpr unsigned PAGE_SHIFT = 12;
constexpr uint64_t PAGE_SIZE = 1ULL << PAGE_SHIFT;

/* Build the allocator from the memory map, minus its reservations, the
 * kernel image and the allocator's own bitmap. False if there's no
 * managed memory (the hosted build). */
bool pmm_init(void);

/* Take memory out of circulation: [start, start + size), page-rounded. */
void pmm_reserve(uint64_t start, uint64_t size);

/* A free physical page, zeroed, or 0 if none. */
uint64_t pmm_alloc(void);
void pmm_free(uint64_t page);

/* 'n' physically contiguous pages, zeroed, or 0. Freed page by page. */
uint64_t pmm_alloc_contig(size_t n);

/* The same, starting at a multiple of 'align' bytes (a power of two). */
uint64_t pmm_alloc_contig_aligned(size_t n, uint64_t align);

/* The same, ending at or below 'limit' (a device's addressing: DMA32). */
uint64_t pmm_alloc_contig_below(size_t n, uint64_t align, uint64_t limit);

/* Pages managed in total, and how many are free now. */
uint64_t pmm_total(void);

/* Where 'bytes' can go at or after 'start': in RAM, clear of the
 * reserved ranges and of [avoid, avoid + avoid_size). 0 if nowhere.
 * pmm_init places its bitmap so; exported for the self-test. */
uint64_t pmm_place(uint64_t start, uint64_t bytes, uint64_t avoid, uint64_t avoid_size);

/* Frees of pages that weren't allocated: always a kernel bug. */
unsigned pmm_bad_frees(void);

/* Steps pmm_alloc_contig's scan has taken (a run of the bitmap each). */
uint64_t pmm_contig_steps(void);
uint64_t pmm_free_pages(void);

/* The longest run of free pages: how fragmented memory is (VQ3). */
uint64_t pmm_largest_free_run(void);

#endif /* KERNEL_PMM_H */
