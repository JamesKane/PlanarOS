/*
 * kernel/kmalloc.h -- small kernel objects
 *
 * Power-of-two size classes from 32 bytes to a page, carved from pages the
 * page allocator hands out. For Areas, mappings and the like; user memory
 * is Areas, never this. From omega's kernel/kmalloc.c, made safe on every
 * CPU.
 */
#ifndef KERNEL_KMALLOC_H
#define KERNEL_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

/* Zeroed memory for an object of 'size' bytes (at most 4,080), or nullptr. */
void *kmalloc(size_t size);
void kfree(void *p);

/* Pages the kernel heap has taken from the page allocator (it keeps them). */
uint64_t kmalloc_pages(void);

#endif /* KERNEL_KMALLOC_H */
