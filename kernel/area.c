/*
 * kernel/area.c -- Areas
 *
 * Identity layout (docs §3.1, VQ3): an Area is one physically contiguous
 * run, at VA = PA; a guard is the run's first page, left unmapped.
 *
 * One lock covers every Area's mappings and every process's list of
 * them; mapping a page may allocate page-table pages, which the page
 * allocator gives with interrupts off.
 *
 * A call that finds an Area by its handle holds a reference until it's
 * done, so the Area outlives a FreeArea racing it; an Area goes when
 * nothing maps it, no handle names it and nobody holds a reference.
 * Mapping re-checks, under the lock, that the handle still names the
 * Area and that the process isn't being reclaimed (docs/10-code-review.md
 * C1).
 *
 * An AREAF_NC Area is mapped uncached in every process that maps it, so
 * no two of its mappings disagree. The kernel's direct map still caches
 * its pages, and is used only to zero them (pmm.c) and to load an
 * image, which is never NC: the alias is flushed as the Area starts and
 * as it ends (arch_dcache_flush).
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "area.h"
#include "futex.h"
#include "handle.h"
#include "kmalloc.h"
#include "kstring.h"
#include "lease.h"
#include "pmm.h"
#include "port.h"
#include "process.h"
#include "sched.h"

constexpr uint32_t AREAF_ALL = AREAF_CLEAR | AREAF_CONTIG | AREAF_EXEC | AREAF_RWX | AREAF_LARGE |
                               AREAF_DMA32 | AREAF_NC;
constexpr uint32_t AREAR_ALL = AREAR_READ | AREAR_WRITE | AREAR_EXEC;
constexpr uint64_t LARGE_ALIGN = 2ULL << 20;

/* One process's view of one Area. */
struct mapping {
    struct area *area;
    struct process *proc;
    uint32_t rights;
    uint64_t exec_bytes;            /* an image's code: read and execute */
    struct mapping *next_in_area, *next_in_proc;
};

struct area {
    uint64_t va, size;              /* bytes, page multiple, guard excluded */
    uint32_t flags;
    bool guard;
    bool freed;                     /* its owner called FreeArea (lock) */
    struct lease *lease;            /* a device's DMA memory (kernel/lease.c) */
    bool held;                      /* lent to a dead driver's device: area_hold_lent */
    bool lent_dma;                  /* held, and its own DMA Area: no lease's once let go */
    bool library;                   /* the shared library region: global, never freed */
    struct area *held_next;
    uint64_t run_pa, run_pages;     /* the whole run, guard included */
    unsigned sharers;
    unsigned refs;                  /* calls using it (lock held) */
    uint32_t handle;                /* 0 if the kernel owns it, or once freed */
    bool user;                      /* AllocArea's: a process's, even once freed or its owner dead */
    struct process *owner;          /* nullptr: the kernel's, or its owner is dead */
    struct mapping *maps;
};

static spinlock_t lock;
static atomic_uint live;

/* Nothing maps it, names it or uses it: free it. Lock held. */
static bool unused(const struct area *a)
{
    return a->sharers == 0 && a->handle == 0 && a->refs == 0;
}

uint64_t area_base(const struct area *a)
{
    return a->va;
}

uint64_t area_size(const struct area *a)
{
    return a->size;
}

uint64_t area_pa(const struct area *a, uint64_t offset)
{
    return a->va + offset;
}

unsigned area_count(void)
{
    return atomic_load(&live);
}

/* --- Backing. --- */

/* area_new, ending at or below 'limit' (0: anywhere): a lease's DMA. */
static struct area *new_below(uint64_t pages, uint32_t flags, bool guard, uint64_t limit)
{
    uint64_t total = pages + guard;
    uint64_t align = flags & AREAF_LARGE ? LARGE_ALIGN : PAGE_SIZE;
    struct area *a;

    if (flags & AREAF_DMA32 && (!limit || limit > 1ULL << 32))
        limit = 1ULL << 32;
    if (pages == 0 || (flags & ~AREAF_ALL))
        return nullptr;
    if (guard && (flags & AREAF_LARGE))
        return nullptr;             /* a guard would break the alignment */
    if (!(a = kmalloc(sizeof *a)))
        return nullptr;
    *a = (struct area){ .size = pages * PAGE_SIZE, .flags = flags, .guard = guard };
    if (!(a->run_pa = pmm_alloc_contig_below(total, align, limit ? limit : ~0ULL))) {
        kfree(a);
        return nullptr;
    }
    a->run_pages = total;
    a->va = a->run_pa + guard * PAGE_SIZE;
    if (flags & AREAF_NC)
        arch_dcache_flush(phys_to_virt(a->run_pa), total * PAGE_SIZE);    /* the zeroes */
    atomic_fetch_add(&live, 1);
    return a;
}

struct area *area_new(uint64_t pages, uint32_t flags, bool guard)
{
    return new_below(pages, flags, guard, 0);
}

static void free_area(struct area *a)
{
    if (a->flags & AREAF_NC)
        arch_dcache_flush(phys_to_virt(a->run_pa), a->run_pages * PAGE_SIZE);
    for (uint64_t i = 0; i < a->run_pages; i++)
        pmm_free(a->run_pa + i * PAGE_SIZE);
    kfree(a);
    atomic_fetch_sub(&live, 1);
}

/* A DMA Area nobody freed goes to its lease's quarantine instead, if
 * the device may still write to it. */
void area_destroy(struct area *a)
{
    if (a->lease && lease_dma_gone(a->lease, a, a->size / PAGE_SIZE, a->freed))
        return;
    free_area(a);
}

void area_unquarantine(struct area *a)
{
    free_area(a);
}

uint64_t area_hold_lent(struct process *p, struct area **list)
{
    unsigned long was = spin_lock_irqsave(&lock);
    uint64_t pages = 0;

    for (struct mapping *m = p->maps; m; m = m->next_in_proc) {
        struct area *a = m->area;

        if (a->held)
            continue;
        if (a->owner == p && a->lease && a->sharers > 1) {
            /* Its own DMA, mapped in a client too (display's buffers):
             * quarantined, but freed only once the client lets go. */
            lease_dma_lent(a->lease, a->size / PAGE_SIZE);
            a->lent_dma = true;
        } else if (a->owner == p || a->lease) {
            continue;
        } else {
            pages += a->size / PAGE_SIZE;
        }
        a->held = true;
        a->refs++;
        a->held_next = *list;
        *list = a;
    }
    spin_unlock_irqrestore(&lock, was);
    return pages;
}

void area_unhold(struct area *list)
{
    while (list) {
        struct area *a = list, *next = a->held_next;
        unsigned long was = spin_lock_irqsave(&lock);
        struct lease *dma = a->lent_dma ? a->lease : nullptr;
        bool last;

        a->held = false;
        a->held_next = nullptr;
        a->refs--;
        if (dma) {
            /* The device is stopped: an ordinary Area now, the client's. */
            a->lease = nullptr;
            a->lent_dma = false;
        }
        last = unused(a);
        spin_unlock_irqrestore(&lock, was);
        if (dma)
            lease_dma_put(dma);
        if (last)
            area_destroy(a);
        list = next;
    }
}

/* --- Mapping. --- */

static uint32_t map_flags(const struct area *a, uint32_t rights)
{
    uint32_t f = a->flags & AREAF_NC ? MAP_USER | MAP_NC : MAP_USER;

    f |= a->library ? MAP_GLOBAL : 0;

    f |= rights & AREAR_READ ? MAP_READ : 0;
    f |= rights & AREAR_WRITE ? MAP_WRITE : 0;
    f |= rights & AREAR_EXEC ? MAP_EXEC : 0;
    return f;
}

/* Map pages of 'm' into its process. Lock held. */
static bool map_pages(struct mapping *m)
{
    struct area *a = m->area;

    for (uint64_t off = 0; off < a->size; off += PAGE_SIZE) {
        uint32_t rights = off < m->exec_bytes ? AREAR_READ | AREAR_EXEC : m->rights;

        if (!arch_map(&m->proc->domain, a->va + off, area_pa(a, off), map_flags(a, rights)))
            return false;
    }
    return true;
}

static void unmap_pages(struct area *a, struct process *p)
{
    for (uint64_t off = 0; off < a->size; off += PAGE_SIZE)
        arch_unmap(&p->domain, a->va + off);
}

/* The mapping of 'a' in 'p', or nullptr. Lock held. */
static struct mapping *find(struct area *a, struct process *p)
{
    for (struct mapping *m = a->maps; m; m = m->next_in_area)
        if (m->proc == p)
            return m;
    return nullptr;
}

/*
 * Unlink 'm' from its Area and its process. Lock held. Anyone waiting on
 * a word in the Area is woken (WaitAddr may return early): the process
 * going may have been the word's only writer, dead now (§12.5), or the
 * waiter may be in it, its word unmapped.
 */
static void unlink_mapping(struct mapping *m)
{
    struct mapping **pp;

    if (futex_waiters())
        futex_wake_range(m->area->va, m->area->size);
    for (pp = &m->area->maps; *pp != m; pp = &(*pp)->next_in_area)
        ;
    *pp = m->next_in_area;
    for (pp = &m->proc->maps; *pp != m; pp = &(*pp)->next_in_proc)
        ;
    *pp = m->next_in_proc;
    m->area->sharers--;
}

/*
 * W^X over the whole Area, not each mapping (review M4): unless it's
 * AREAF_RWX, no domain may write it while any domain may execute it.
 * 'except' is the mapping about to change. Lock held.
 */
static bool wx_conflict(const struct area *a, const struct mapping *except, uint32_t rights)
{
    if (a->flags & AREAF_RWX)
        return false;
    for (const struct mapping *m = a->maps; m; m = m->next_in_area) {
        uint32_t r = m->rights | (m->exec_bytes ? AREAR_EXEC : 0);

        if (m != except && (((rights & AREAR_WRITE) && (r & AREAR_EXEC)) ||
                            ((rights & AREAR_EXEC) && (r & AREAR_WRITE))))
            return true;
    }
    return false;
}

/* Map 'a' into 'p'. With 'handle', only while that handle still names
 * 'a' (a racing FreeArea makes it ERR_BADHANDLE). Never into a process
 * being reclaimed. */
static int64_t map_into(struct area *a, struct process *p, uint32_t rights, uint64_t exec_bytes,
                        uint32_t handle)
{
    struct mapping *m = kmalloc(sizeof *m);

    if (!m)
        return ERR_NOMEM;
    unsigned long was = spin_lock_irqsave(&lock);
    if ((handle && a->handle != handle) || p->maps_closed) {
        spin_unlock_irqrestore(&lock, was);
        kfree(m);
        return handle && a->handle != handle ? ERR_BADHANDLE : ERR_NOTFOUND;
    }
    if (find(a, p) || wx_conflict(a, nullptr, rights)) {
        bool exists = find(a, p) != nullptr;

        spin_unlock_irqrestore(&lock, was);
        kfree(m);
        return exists ? ERR_EXISTS : ERR_RIGHTS;
    }
    *m = (struct mapping){ .area = a, .proc = p, .rights = rights, .exec_bytes = exec_bytes,
                           .next_in_area = a->maps, .next_in_proc = p->maps };
    a->maps = m;
    p->maps = m;
    a->sharers++;
    if (!map_pages(m)) {
        unmap_pages(a, p);
        unlink_mapping(m);
        spin_unlock_irqrestore(&lock, was);
        kfree(m);
        return ERR_NOMEM;
    }
    spin_unlock_irqrestore(&lock, was);
    return 0;
}

void area_make_library(struct area *a)
{
    unsigned long was = spin_lock_irqsave(&lock);

    a->library = true;
    a->refs++;                      /* the kernel's, for good */
    spin_unlock_irqrestore(&lock, was);
    atomic_fetch_sub(&live, 1);     /* not one a leak check counts */
}

uint64_t area_process_pages(struct process *p, uint64_t *library)
{
    unsigned long was = spin_lock_irqsave(&lock);
    uint64_t bytes = 0, lib = 0;

    for (const struct mapping *m = p->maps; m; m = m->next_in_proc) {
        bytes += m->area->size;
        lib += m->area->library ? m->area->size : 0;
    }
    spin_unlock_irqrestore(&lock, was);
    *library = lib / PAGE_SIZE;
    return bytes / PAGE_SIZE;
}

bool area_map(struct area *a, struct process *p, uint32_t rights, uint64_t exec_bytes)
{
    return map_into(a, p, rights, exec_bytes, 0) == 0;
}

int64_t area_share(struct area *a, struct process *p, uint32_t rights)
{
    return map_into(a, p, rights, 0, 0);
}

void area_release(struct area *a, struct process *p)
{
    unsigned long was = spin_lock_irqsave(&lock);
    struct mapping *m = find(a, p);
    bool last;

    if (!m) {
        spin_unlock_irqrestore(&lock, was);
        return;
    }
    unmap_pages(a, p);
    unlink_mapping(m);
    last = unused(a);
    spin_unlock_irqrestore(&lock, was);
    kfree(m);
    if (last)
        area_destroy(a);
}

void area_process_died(struct process *p)
{
    unsigned long w = spin_lock_irqsave(&lock);

    p->maps_closed = true;          /* nothing maps into it from now on */
    spin_unlock_irqrestore(&lock, w);
    for (;;) {
        unsigned long was = spin_lock_irqsave(&lock);
        struct mapping *m = p->maps;
        struct area *a;
        bool last;

        if (!m) {
            spin_unlock_irqrestore(&lock, was);
            return;
        }
        a = m->area;
        unlink_mapping(m);
        if (a->owner == p) {
            a->handle = 0;          /* its handle goes with the process */
            a->owner = nullptr;     /* and the record, which another may reuse */
        }
        last = unused(a);
        spin_unlock_irqrestore(&lock, was);
        kfree(m);
        if (last)
            area_destroy(a);
    }
}

bool area_map_phys(struct process *p, uint64_t pa, uint64_t size, uint32_t flags)
{
    unsigned long was = spin_lock_irqsave(&lock);
    uint64_t off = 0;

    for (; off < size; off += PAGE_SIZE)
        if (!arch_map(&p->domain, pa + off, pa + off, flags))
            break;
    if (off < size)
        while (off) {
            off -= PAGE_SIZE;
            arch_unmap(&p->domain, pa + off);
        }
    spin_unlock_irqrestore(&lock, was);
    return off >= size;
}

void area_unmap_phys(struct process *p, uint64_t pa, uint64_t size)
{
    unsigned long was = spin_lock_irqsave(&lock);

    for (uint64_t off = 0; off < size; off += PAGE_SIZE)
        arch_unmap(&p->domain, pa + off);
    spin_unlock_irqrestore(&lock, was);
}

bool area_map_domain(struct area *a, struct arch_domain *d)
{
    for (uint64_t off = 0; off < a->size; off += PAGE_SIZE)
        if (!arch_map(d, a->va + off, area_pa(a, off), MAP_USER | MAP_READ | MAP_WRITE))
            return false;
    return true;
}

void area_unmap_domain(struct area *a, struct arch_domain *d)
{
    for (uint64_t off = 0; off < a->size; off += PAGE_SIZE)
        arch_unmap(d, a->va + off);
}

void area_copy_in(struct area *a, uint64_t offset, const void *src, uint64_t len)
{
    const uint8_t *s = src;

    while (len) {
        uint64_t chunk = PAGE_SIZE - offset % PAGE_SIZE;

        if (chunk > len)
            chunk = len;
        memcpy(phys_to_virt(area_pa(a, offset)), s, chunk);
        offset += chunk;
        s += chunk;
        len -= chunk;
    }
}

bool area_fault_retry(uint64_t va, uint32_t flags)
{
    struct task *self = task_self();
    bool ok;

    if (!self->domain)
        return false;
    unsigned long was = spin_lock_irqsave(&lock);
    ok = arch_allows(self->domain, va, flags);   /* no change part way through now */
    spin_unlock_irqrestore(&lock, was);
    return ok;
}

/* --- The kernel calls. --- */

/* Under the handle table's lock, so the Area can't go first. */
static void get_area(void *obj, void *arg)
{
    struct area *a = obj;
    unsigned long was = spin_lock_irqsave(&lock);

    a->refs++;
    spin_unlock_irqrestore(&lock, was);
    *(struct area **)arg = a;
}

/* The Area 'h' names, with a reference for put_area(); nullptr if none. */
static struct area *area_of(uint32_t h)
{
    struct area *a = nullptr;

    handle_with(h, HANDLE_AREA, get_area, &a);
    return a;
}

static void put_area(struct area *a)
{
    unsigned long was = spin_lock_irqsave(&lock);
    bool last = --a->refs == 0 && unused(a);

    spin_unlock_irqrestore(&lock, was);
    if (last)
        area_destroy(a);
}

/* A task handle names a live task, so its process is still there; the
 * reference keeps the record (process_put). */
static void get_process(void *obj, void *arg)
{
    struct process *p = ((struct task *)obj)->process;

    if (p)
        process_get(p);
    *(struct process **)arg = p;
}

static struct process *process_of(uint32_t task)
{
    struct process *p = nullptr;

    handle_with(task, HANDLE_TASK, get_process, &p);
    return p;
}

/* Are 'rights' allowed by the Area's flags? Execute needs AREAF_EXEC,
 * write and execute at once AREAF_RWX, and write needs read. */
static int64_t check_rights(const struct area *a, uint32_t rights)
{
    if ((rights & ~AREAR_ALL) || !(rights & AREAR_READ))
        return ERR_BADARG;
    if ((rights & AREAR_EXEC) && !(a->flags & (AREAF_EXEC | AREAF_RWX)))
        return ERR_RIGHTS;
    if ((rights & AREAR_EXEC) && (rights & AREAR_WRITE) && !(a->flags & AREAF_RWX))
        return ERR_RIGHTS;
    return 0;
}

/* AllocArea, and AllocDmaArea with the lease 'l'. */
static int64_t alloc(uint64_t size, uint32_t flags, uint64_t addr_out, struct lease *l)
{
    struct process *p = task_self()->process;
    struct area *a;
    uint32_t h;
    uint64_t va;
    int64_t err;

    if (size == 0 || (flags & ~AREAF_ALL))
        return ERR_BADARG;
    if ((flags & AREAF_NC) && (flags & (AREAF_EXEC | AREAF_RWX)))
        return ERR_BADARG;          /* code is fetched through the caches */
    if ((flags & AREAF_NC) && !arch_has_nc())
        return ERR_NOSYS;
    if (size / PAGE_SIZE + (size % PAGE_SIZE != 0) > pmm_free_pages())
        return ERR_NOMEM;           /* can't be had: say so before zeroing it all */
    if (!(a = new_below((size + PAGE_SIZE - 1) / PAGE_SIZE, flags, false,
                        l ? lease_dma_limit(l) : 0)))
        return ERR_NOMEM;
    a->owner = p;
    a->user = true;
    a->refs = 1;                    /* ours, until the call is done */
    if (l) {
        a->lease = l;
        lease_dma_new(l, a->size / PAGE_SIZE);
    }
    if (!(h = a->handle = handle_new(HANDLE_AREA, a, p))) {
        a->freed = true;
        area_destroy(a);
        return handle_error(p);
    }
    /* From here a sibling task may FreeArea(h) under us. */
    if ((err = map_into(a, p, AREAR_READ | AREAR_WRITE, 0, h)) != 0) {
        if (err != ERR_BADHANDLE)
            area_free_call(h);
        put_area(a);
        return err;
    }
    va = a->va;
    put_area(a);
    if (!arch_copy_to_user(addr_out, &va, sizeof va)) {
        area_free_call(h);
        return ERR_BADADDR;
    }
    return h;
}

int64_t area_alloc_call(uint64_t size, uint32_t flags, uint64_t addr_out)
{
    return alloc(size, flags, addr_out, nullptr);
}

int64_t area_alloc_dma(struct lease *l, uint64_t size, uint32_t flags, uint64_t addr_out)
{
    return alloc(size, flags, addr_out, l);
}

/* The owner gives up its handle and its own mapping; the Area lives on
 * while anyone else maps it. */
int64_t area_free_call(uint32_t h)
{
    struct process *p = task_self()->process;
    struct area *a = area_of(h);
    bool mine;

    if (!a)
        return ERR_BADHANDLE;
    unsigned long was = spin_lock_irqsave(&lock);
    if ((mine = a->owner == p && a->handle == h)) {
        a->handle = 0;              /* only one FreeArea gets here */
        a->freed = true;
    }
    spin_unlock_irqrestore(&lock, was);
    if (mine) {
        handle_free(h);
        area_release(a, p);
    }
    put_area(a);
    return mine ? 0 : ERR_BADHANDLE;
}

struct area *area_take(uint32_t h, struct process *p, uint64_t min_size)
{
    struct area *a = area_of(h);
    struct mapping *m = nullptr;
    bool ok;

    if (!a)
        return nullptr;
    unsigned long was = spin_lock_irqsave(&lock);
    ok = a->owner == p && a->handle == h && !a->lease && !a->held && a->flags & AREAF_EXEC &&
         a->size >= min_size && a->sharers == 1 && (m = find(a, p)) && a->refs == 1;
    if (ok) {
        unmap_pages(a, p);
        unlink_mapping(m);
        a->handle = 0;
        a->owner = nullptr;         /* the kernel's, until it maps it in the child */
        a->user = false;
        a->refs--;                  /* area_of's: now as area_new leaves one */
    }
    spin_unlock_irqrestore(&lock, was);
    if (!ok) {
        put_area(a);
        return nullptr;
    }
    handle_free(h);
    kfree(m);
    return a;
}

/* What the Area holds, made visible to instruction fetch on every CPU. */
static void sync_code(struct area *a)
{
    for (uint64_t off = 0; off < a->size; off += PAGE_SIZE)
        arch_sync_icache(phys_to_virt(area_pa(a, off)), PAGE_SIZE);
}

/* Shared executable, it's synced as ProtectArea syncs: the owner may have
 * written it through a mapping it has since made read-only (§10.8). */
int64_t area_share_call(uint32_t h, uint32_t task, uint32_t rights)
{
    struct process *self = task_self()->process;
    struct area *a = area_of(h);
    struct process *target = process_of(task);
    int64_t err;

    if (!a || a->owner != self)
        err = ERR_BADHANDLE;
    else if (!target)
        err = ERR_NOTFOUND;
    else if (target != self && !ipc_connected(self, target))
        err = ERR_RIGHTS;           /* only to a peer: review M10 */
    else if (!(err = check_rights(a, rights)) && !(err = map_into(a, target, rights, 0, h)) &&
             (rights & AREAR_EXEC))
        sync_code(a);
    if (a)
        put_area(a);
    if (target)
        process_put(target);
    return err;
}

int64_t area_unshare_call(uint32_t h, uint32_t task)
{
    struct area *a = area_of(h);
    struct process *target = process_of(task);
    int64_t err = 0;

    if (!a || a->owner != task_self()->process) {
        err = ERR_BADHANDLE;
    } else if (!target || target == a->owner) {
        err = ERR_NOTFOUND;
    } else {
        unsigned long was = spin_lock_irqsave(&lock);
        bool mapped = find(a, target) != nullptr;

        spin_unlock_irqrestore(&lock, was);
        if (mapped)
            area_release(a, target);
        else
            err = ERR_NOTFOUND;
    }
    if (a)
        put_area(a);
    if (target)
        process_put(target);
    return err;
}

/* ReleaseArea: the caller's own mapping of an Area another process
 * owns and shared with it (v5-plan step 5: a server done with what a
 * client lent it), or did before it freed it or died (v7-plan step 5: a
 * killed client's windows). The Area goes, if nothing else holds it.
 * Never one the kernel made: an arena, a stack, an image. */
int64_t area_release_call(uint64_t addr)
{
    struct process *p = task_self()->process;
    struct area *a = nullptr;
    unsigned long was = spin_lock_irqsave(&lock);

    for (struct mapping *m = p->maps; m; m = m->next_in_proc)
        if (addr >= m->area->va && addr - m->area->va < m->area->size) {
            a = m->area;
            break;
        }
    if (!a || a->owner == p || !a->user) {
        spin_unlock_irqrestore(&lock, was);
        return a ? ERR_RIGHTS : ERR_NOTFOUND;
    }
    a->refs++;
    spin_unlock_irqrestore(&lock, was);
    area_release(a, p);
    put_area(a);
    return 0;
}

int64_t area_protect_call(uint32_t h, uint32_t rights)
{
    struct process *p = task_self()->process;
    struct area *a = area_of(h);
    struct mapping *m = nullptr;
    int64_t err;

    if (!a)
        return ERR_BADHANDLE;
    if (a->owner != p) {
        put_area(a);
        return ERR_BADHANDLE;
    }
    if ((err = check_rights(a, rights))) {
        put_area(a);
        return err;
    }
    unsigned long was = spin_lock_irqsave(&lock);
    if (a->handle == h && (m = find(a, p))) {
        if (wx_conflict(a, m, rights)) {
            spin_unlock_irqrestore(&lock, was);
            put_area(a);
            return ERR_RIGHTS;
        }
        m->rights = rights;
        map_pages(m);               /* remaps every page: arch_map breaks before making */
    }
    spin_unlock_irqrestore(&lock, was);
    if (m && (rights & AREAR_EXEC))
        sync_code(a);
    put_area(a);
    return m ? 0 : ERR_NOTFOUND;
}

int64_t area_query_call(uint64_t addr, uint64_t info_out)
{
    struct process *p = task_self()->process;
    struct AreaInfo info = { 0 };
    bool found = false;

    unsigned long was = spin_lock_irqsave(&lock);
    for (struct mapping *m = p->maps; m; m = m->next_in_proc) {
        struct area *a = m->area;

        if (addr >= a->va && addr - a->va < a->size) {
            info = (struct AreaInfo){
                .ai_Address = (void *)(uintptr_t)a->va, .ai_Size = a->size,
                .ai_Area = a->owner == p ? a->handle : 0, .ai_Rights = m->rights,
                .ai_Flags = a->flags, .ai_Sharers = a->sharers,
            };
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&lock, was);
    if (!found)
        return ERR_NOTFOUND;
    return arch_copy_to_user(info_out, &info, sizeof info) ? 0 : ERR_BADADDR;
}
