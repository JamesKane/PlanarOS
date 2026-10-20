/*
 * kernel/area.c -- Areas
 *
 * Identity layout: one physically contiguous run, at VA = PA; a guard is
 * the run's first page, left unmapped. Global layout: a range of
 * kernel/va.c's window (with its guard page) backed by pages from
 * anywhere, listed in 'pages', or one contiguous run with AREAF_CONTIG.
 *
 * One lock covers every Area's mappings and every process's list of
 * them; mapping a page may allocate page-table pages, which the page
 * allocator gives with interrupts off.
 *
 * Not yet safe: two tasks of one process using and freeing the same Area
 * handle at once (the calls look the Area up, then act on it). Handles
 * will hold references when IPC needs them (V2).
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "area.h"
#include "handle.h"
#include "kmalloc.h"
#include "kstring.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "va.h"

constexpr uint32_t AREAF_ALL = AREAF_CLEAR | AREAF_CONTIG | AREAF_EXEC | AREAF_RWX | AREAF_LARGE;
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
    enum layout layout;
    bool guard;
    uint64_t pa;                    /* a contiguous Area's first mapped page */
    uint64_t *pages;                /* a scattered one's pages, by index */
    uint64_t pages_pa;              /* where 'pages' lives, if it took whole pages */
    uint64_t run_pa, run_pages;     /* identity: the whole run, guard included */
    unsigned sharers;
    uint32_t handle;                /* 0 if the kernel owns it, or once freed */
    struct process *owner;
    struct mapping *maps;
};

static spinlock_t lock;
static atomic_uint live;

static uint64_t npages(const struct area *a)
{
    return a->size / PAGE_SIZE;
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
    uint64_t i = offset / PAGE_SIZE;

    return (a->pages ? a->pages[i] : a->pa + i * PAGE_SIZE) + offset % PAGE_SIZE;
}

unsigned area_count(void)
{
    return atomic_load(&live);
}

/* --- Backing, per layout. --- */

static bool back_identity(struct area *a, uint64_t n)
{
    uint64_t total = n + a->guard;
    uint64_t align = a->flags & AREAF_LARGE ? LARGE_ALIGN : PAGE_SIZE;

    if (a->guard && (a->flags & AREAF_LARGE))
        return false;               /* a guard would break the alignment */
    if (!(a->run_pa = pmm_alloc_contig_aligned(total, align)))
        return false;
    a->run_pages = total;
    a->pa = a->run_pa + a->guard * PAGE_SIZE;
    a->va = a->pa;
    return true;
}

static void unback_identity(struct area *a)
{
    for (uint64_t i = 0; i < a->run_pages; i++)
        pmm_free(a->run_pa + i * PAGE_SIZE);
}

static bool back_global(struct area *a, uint64_t n)
{
    uint64_t align = a->flags & AREAF_LARGE ? LARGE_ALIGN : PAGE_SIZE;
    uint64_t bytes = n * sizeof(uint64_t);

    if (!(a->va = va_alloc(n * PAGE_SIZE, align)))
        return false;
    if (a->flags & AREAF_CONTIG) {
        if ((a->pa = pmm_alloc_contig(n)))
            return true;
    } else {
        /* The page list: from the kernel heap, or whole pages if long. */
        if (bytes <= 2048)
            a->pages = kmalloc(bytes);
        else if ((a->pages_pa = pmm_alloc_contig((bytes + PAGE_SIZE - 1) / PAGE_SIZE)))
            a->pages = phys_to_virt(a->pages_pa);
        for (uint64_t i = 0; a->pages && i < n; i++) {
            if (!(a->pages[i] = pmm_alloc())) {
                while (i--)
                    pmm_free(a->pages[i]);
                break;
            }
            if (i == n - 1)
                return true;
        }
    }
    va_free(a->va - PAGE_SIZE, (n + 1) * PAGE_SIZE);
    return false;
}

static void free_page_list(struct area *a)
{
    uint64_t n = npages(a);

    if (a->pages_pa) {
        uint64_t list_pages = (n * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;

        for (uint64_t i = 0; i < list_pages; i++)
            pmm_free(a->pages_pa + i * PAGE_SIZE);
    } else {
        kfree(a->pages);
    }
    a->pages = nullptr;
}

static void unback_global(struct area *a)
{
    uint64_t n = npages(a);

    for (uint64_t i = 0; i < n; i++)
        pmm_free(area_pa(a, i * PAGE_SIZE));
    if (a->pages)
        free_page_list(a);
    va_free(a->va - PAGE_SIZE, (n + 1) * PAGE_SIZE);
}

struct area *area_new(enum layout l, uint64_t pages, uint32_t flags, bool guard)
{
    struct area *a;
    bool ok;

    if (pages == 0 || (flags & ~AREAF_ALL))
        return nullptr;
    if (!(a = kmalloc(sizeof *a)))
        return nullptr;
    *a = (struct area){ .size = pages * PAGE_SIZE, .flags = flags, .layout = l, .guard = guard };
    ok = l == LAYOUT_IDENTITY ? back_identity(a, pages) : back_global(a, pages);
    if (!ok) {
        kfree(a);
        return nullptr;
    }
    atomic_fetch_add(&live, 1);
    return a;
}

void area_destroy(struct area *a)
{
    if (a->layout == LAYOUT_IDENTITY)
        unback_identity(a);
    else
        unback_global(a);
    kfree(a);
    atomic_fetch_sub(&live, 1);
}

/* --- Mapping. --- */

static uint32_t map_flags(uint32_t rights)
{
    uint32_t f = MAP_USER;

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

        if (!arch_map(&m->proc->domain, a->va + off, area_pa(a, off), map_flags(rights)))
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

/* Unlink 'm' from its Area and its process. Lock held. */
static void unlink_mapping(struct mapping *m)
{
    struct mapping **pp;

    for (pp = &m->area->maps; *pp != m; pp = &(*pp)->next_in_area)
        ;
    *pp = m->next_in_area;
    for (pp = &m->proc->maps; *pp != m; pp = &(*pp)->next_in_proc)
        ;
    *pp = m->next_in_proc;
    m->area->sharers--;
}

static int64_t map_into(struct area *a, struct process *p, uint32_t rights, uint64_t exec_bytes)
{
    struct mapping *m = kmalloc(sizeof *m);

    if (!m)
        return ERR_NOMEM;
    unsigned long was = spin_lock_irqsave(&lock);
    if (find(a, p)) {
        spin_unlock_irqrestore(&lock, was);
        kfree(m);
        return ERR_EXISTS;
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

bool area_map(struct area *a, struct process *p, uint32_t rights, uint64_t exec_bytes)
{
    return map_into(a, p, rights, exec_bytes) == 0;
}

int64_t area_share(struct area *a, struct process *p, uint32_t rights)
{
    return map_into(a, p, rights, 0);
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
    last = a->sharers == 0 && a->handle == 0;
    spin_unlock_irqrestore(&lock, was);
    kfree(m);
    if (last)
        area_destroy(a);
}

void area_process_died(struct process *p)
{
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
        if (a->owner == p)
            a->handle = 0;          /* its handle goes with the process */
        last = a->sharers == 0 && a->handle == 0;
        spin_unlock_irqrestore(&lock, was);
        kfree(m);
        if (last)
            area_destroy(a);
    }
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

/* --- The kernel calls. --- */

static void get_area(void *obj, void *arg)
{
    *(struct area **)arg = obj;
}

static struct area *area_of(uint32_t h)
{
    struct area *a = nullptr;

    handle_with(h, HANDLE_AREA, get_area, &a);
    return a;
}

static void get_process(void *obj, void *arg)
{
    *(struct process **)arg = ((struct task *)obj)->process;
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

int64_t area_alloc_call(uint64_t size, uint32_t flags, uint64_t addr_out)
{
    struct process *p = task_self()->process;
    struct area *a;
    uint64_t va;

    if (size == 0 || size > (VA_WINDOW_TOP - VA_WINDOW_BASE) / 2 || (flags & ~AREAF_ALL))
        return ERR_BADARG;
    if (!(a = area_new(layout_boot(), (size + PAGE_SIZE - 1) / PAGE_SIZE, flags, false)))
        return ERR_NOMEM;
    if (!(a->handle = handle_new(HANDLE_AREA, a, p))) {
        area_destroy(a);
        return ERR_NOMEM;
    }
    a->owner = p;
    if (map_into(a, p, AREAR_READ | AREAR_WRITE, 0) != 0) {
        handle_free(a->handle);
        area_destroy(a);
        return ERR_NOMEM;
    }
    va = a->va;
    if (!arch_copy_to_user(addr_out, &va, sizeof va)) {
        area_free_call(a->handle);
        return ERR_BADADDR;
    }
    return a->handle;
}

/* The owner gives up its handle and its own mapping; the Area lives on
 * while anyone else maps it. */
int64_t area_free_call(uint32_t h)
{
    struct process *p = task_self()->process;
    struct area *a = area_of(h);

    if (!a || a->owner != p)
        return ERR_BADHANDLE;
    handle_free(h);
    unsigned long was = spin_lock_irqsave(&lock);
    a->handle = 0;
    bool unmapped = a->sharers == 0;
    spin_unlock_irqrestore(&lock, was);
    if (unmapped)
        area_destroy(a);
    else
        area_release(a, p);
    return 0;
}

int64_t area_share_call(uint32_t h, uint32_t task, uint32_t rights)
{
    struct area *a = area_of(h);
    struct process *target = process_of(task);
    int64_t err;

    if (!a || a->owner != task_self()->process)
        return ERR_BADHANDLE;
    if (!target)
        return ERR_NOTFOUND;
    if ((err = check_rights(a, rights)))
        return err;
    return map_into(a, target, rights, 0);
}

int64_t area_unshare_call(uint32_t h, uint32_t task)
{
    struct area *a = area_of(h);
    struct process *target = process_of(task);
    bool mapped;

    if (!a || a->owner != task_self()->process)
        return ERR_BADHANDLE;
    if (!target || target == a->owner)
        return ERR_NOTFOUND;
    unsigned long was = spin_lock_irqsave(&lock);
    mapped = find(a, target) != nullptr;
    spin_unlock_irqrestore(&lock, was);
    if (!mapped)
        return ERR_NOTFOUND;
    area_release(a, target);
    return 0;
}

int64_t area_protect_call(uint32_t h, uint32_t rights)
{
    struct process *p = task_self()->process;
    struct area *a = area_of(h);
    struct mapping *m;
    int64_t err;

    if (!a || a->owner != p)
        return ERR_BADHANDLE;
    if ((err = check_rights(a, rights)))
        return err;
    unsigned long was = spin_lock_irqsave(&lock);
    if ((m = find(a, p))) {
        m->rights = rights;
        map_pages(m);               /* remaps every page: arch_map breaks before making */
    }
    spin_unlock_irqrestore(&lock, was);
    if (rights & AREAR_EXEC)
        for (uint64_t off = 0; off < a->size; off += PAGE_SIZE)
            arch_sync_icache(phys_to_virt(area_pa(a, off)), PAGE_SIZE);
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
