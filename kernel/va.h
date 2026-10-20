/*
 * kernel/va.h -- the global layout's address allocator
 *
 * The global layout (docs §3.1, VQ3) hands out ranges of the single
 * address space from a window of its own, system-wide, so an address
 * means the same thing in every domain that maps it. The window lies
 * above every board's RAM, so its addresses never meet the identity
 * layout's (which are physical). Each range comes with an unmapped guard
 * page in front, so running off the end of one Area, or off the bottom
 * of a stack, faults instead of landing in the next. From omega's
 * kernel/va.c.
 */
#ifndef KERNEL_VA_H
#define KERNEL_VA_H

#include <stdint.h>

constexpr uint64_t VA_WINDOW_BASE = 0x0000001000000000ULL;     /* 64 GiB */
constexpr uint64_t VA_WINDOW_TOP  = 0x0000002000000000ULL;     /* 128 GiB */

/* 'bytes' (page-rounded) at an address that is a multiple of 'align' (a
 * power of two, at least a page), with the page in front reserved too.
 * 0 if the window is full. */
uint64_t va_alloc(uint64_t bytes, uint64_t align);

/* Give back [start, start + bytes), page-rounded: a range va_alloc
 * reserved, guard page included. */
void va_free(uint64_t start, uint64_t bytes);

/* Bytes not reserved, for leak checks. */
uint64_t va_free_bytes(void);

#endif /* KERNEL_VA_H */
