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

/* Pages managed in total, and how many are free now. */
uint64_t pmm_total(void);
uint64_t pmm_free_pages(void);

/* The longest run of free pages: how fragmented memory is (VQ3). */
uint64_t pmm_largest_free_run(void);

#endif /* KERNEL_PMM_H */
