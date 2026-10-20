/*
 * kernel/area.h -- Areas: the kernel's only memory abstraction
 *
 * docs §3.3. An Area is a page-granular range of the single address
 * space with committed memory behind it, mapped into one or more
 * processes, each with its own rights. A process's image, its stacks and
 * the Areas it allocates are all Areas. An Area lives while anyone maps
 * it; the last one out frees it. Where its memory comes from is the
 * layout's business (kernel/layout.h), and this file is the only one
 * that knows.
 *
 * The calls taking handles act for the running task, from its kernel call.
 */
#ifndef KERNEL_AREA_H
#define KERNEL_AREA_H

#include <stdint.h>

#include "layout.h"

struct area;
struct arch_domain;
struct process;

/*
 * A new Area of 'pages' pages from layout 'l'. With 'guard', an
 * unmapped page sits below it (stacks): a range of addresses in the
 * global layout, a page of memory in the identity layout. nullptr if
 * there's no memory or no room.
 */
struct area *area_new(enum layout l, uint64_t pages, uint32_t flags, bool guard);

/* Free an Area nobody maps (the soak test's, or one never mapped). */
void area_destroy(struct area *a);

/* Map 'a' into 'p' with 'rights' (AREAR_*). An image passes the bytes of
 * code at its start, mapped read and execute whatever 'rights' says. */
bool area_map(struct area *a, struct process *p, uint32_t rights, uint64_t exec_bytes);

/* 'p' stops mapping 'a'; it's freed if nobody else maps it. */
void area_release(struct area *a, struct process *p);

/* A process died: drop all its mappings and free what nobody maps now.
 * Its domain is being destroyed, so nothing is unmapped page by page. */
void area_process_died(struct process *p);

/* Where an Area starts, how big it is, and the physical address of the
 * page at byte 'offset' (the kernel reaches it through the direct map). */
uint64_t area_base(const struct area *a);
uint64_t area_size(const struct area *a);
uint64_t area_pa(const struct area *a, uint64_t offset);

/* Copy 'len' bytes from 'src' into 'a' at 'offset', through the direct map. */
void area_copy_in(struct area *a, uint64_t offset, const void *src, uint64_t len);

/* Map every page of 'a' into a bare domain (the soak test's page-table
 * count), and take them out again. */
bool area_map_domain(struct area *a, struct arch_domain *d);
void area_unmap_domain(struct area *a, struct arch_domain *d);

/* A user fault at 'va': is the access (MAP_READ/WRITE/EXEC) allowed after
 * all, now that no mapping is part way through changing? A page being
 * remapped is briefly absent (break before make); a task touching it
 * then retries rather than dying (review M6). */
bool area_fault_retry(uint64_t va, uint32_t flags);

/* Areas in existence, for leak checks. */
unsigned area_count(void);

/* The kernel calls. */
int64_t area_alloc_call(uint64_t size, uint32_t flags, uint64_t addr_out);
int64_t area_free_call(uint32_t area);
int64_t area_share_call(uint32_t area, uint32_t task, uint32_t rights);
int64_t area_unshare_call(uint32_t area, uint32_t task);
int64_t area_protect_call(uint32_t area, uint32_t rights);
int64_t area_query_call(uint64_t addr, uint64_t info_out);

/* Sharing between two processes, for the kernel's own tests and later
 * the IPC connections (V2). */
int64_t area_share(struct area *a, struct process *p, uint32_t rights);

#endif /* KERNEL_AREA_H */
