/*
 * kernel/area.h -- Areas: the kernel's only memory abstraction
 *
 * docs §3.3. An Area is a page-granular range of the single address
 * space with committed memory behind it, mapped into one or more
 * processes, each with its own rights. A process's image, its stacks and
 * the Areas it allocates are all Areas. An Area lives while anyone maps
 * it; the last one out frees it. Its memory is one physically contiguous
 * run, mapped at VA = PA (the identity layout, docs §3.1, VQ3).
 *
 * The calls taking handles act for the running task, from its kernel call.
 */
#ifndef KERNEL_AREA_H
#define KERNEL_AREA_H

#include <stdint.h>

struct area;
struct arch_domain;
struct process;

/*
 * A new Area of 'pages' pages. With 'guard', an unmapped page of memory
 * sits below it (stacks). nullptr if there's no run of free pages that
 * long.
 */
struct area *area_new(uint64_t pages, uint32_t flags, bool guard);

/* Free an Area nobody maps (the soak test's, or one never mapped). A
 * DMA Area its owner never freed may go to its lease's quarantine
 * instead; area_unquarantine frees it from there. */
void area_destroy(struct area *a);
void area_unquarantine(struct area *a);

/*
 * A dying driver's lent memory (v4-plan step 5): every Area 'p' maps
 * that is neither its own nor a DMA Area, such as a connection's arena
 * holding a client's buffers, which its device may still be writing
 * into. Each is held, linked on '*list', and not freed until
 * area_unhold, whoever lets go of it meanwhile. One already held for
 * another lease is left to that one. The pages held.
 *
 * Its own DMA Areas that a client maps too (display's buffers, v5-plan
 * step 3) are held the same way, but counted as its lease's quarantine
 * (lease_dma_lent): its device may still read them. area_unhold makes
 * each an ordinary Area, freed when the client lets go.
 *
 * The caller holds the lease lock (lease.c's, which comes before this
 * file's).
 */
uint64_t area_hold_lent(struct process *p, struct area **list);
void area_unhold(struct area *list);

/* Map 'a' into 'p' with 'rights' (AREAR_*). An image passes the bytes of
 * code at its start, mapped read and execute whatever 'rights' says. */
bool area_map(struct area *a, struct process *p, uint32_t rights, uint64_t exec_bytes);

/* Take Area 'h' from 'p', which owns and alone maps it, and which made it
 * AREAF_EXEC and at least 'min_size' long: no longer mapped in 'p' or
 * named by its handle, for the kernel to map in a new process
 * (CreateProcessImage). nullptr, and nothing changed, if it can't be. */
struct area *area_take(uint32_t h, struct process *p, uint64_t min_size);

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
int64_t area_release_call(uint64_t addr);
int64_t area_protect_call(uint32_t area, uint32_t rights);
int64_t area_query_call(uint64_t addr, uint64_t info_out);

/* Map [pa, pa + size) at VA = PA into 'p' with 'flags' (MAP_*), or take
 * it out again: a lease's registers, or the firmware's tables. Under the
 * lock that covers every mapping, so it can't race an Area being mapped
 * into the same domain. False, with nothing mapped, if it can't. */
bool area_map_phys(struct process *p, uint64_t pa, uint64_t size, uint32_t flags);
void area_unmap_phys(struct process *p, uint64_t pa, uint64_t size);

/* AllocDmaArea's work, once kernel/lease.c has checked the lease. */
struct lease;
int64_t area_alloc_dma(struct lease *l, uint64_t size, uint32_t flags, uint64_t addr_out);

/* Sharing between two processes, for the kernel's own tests and later
 * the IPC connections (V2). */
int64_t area_share(struct area *a, struct process *p, uint32_t rights);

#endif /* KERNEL_AREA_H */
