/*
 * kernel/lease.c -- device leases
 *
 * One lock covers every lease, its lines and its quarantine. The calls
 * find a lease by its handle under the lock, since the grantee doesn't
 * own the handle; the lease's owner frees the handle before the lease
 * can go. A lease lives while its owner's handle, its grant or any of
 * its DMA Areas (live or quarantined) needs it.
 *
 * Interrupts: a bound line is delivered to the bound task as a signal.
 * It is masked when it fires and unmasked by AckWaitIrq, so a level-
 * triggered line can't storm before the driver has dealt with it.
 *
 * Lock order: this one, then area.c's (which maps and unmaps the
 * registers), then the run queues. area.c offers a DMA Area back unlocked.
 */
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "acpi.h"
#include "arch.h"
#include "area.h"
#include "devmem.h"
#include "fdt.h"
#include "handle.h"
#include "intc.h"
#include "irq.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "lease.h"
#include "memmap.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "spinlock.h"
#include "syscall.h"

constexpr unsigned IRQ_LEVEL = 3;               /* drivers' lines */
constexpr uint64_t LEASE_MAX = 1ULL << 30;

struct line {
    uint32_t source;
    bool bound;
    bool masked;                    /* it fired, and AckWaitIrq hasn't run */
    struct task *task;              /* bound to (lock) */
    uint64_t sig;
};

struct quarantined {
    struct area *area;
    uint64_t pages;
    struct quarantined *next;
};

struct lease {
    uint64_t base, size;
    struct line line[LEASE_IRQS];
    unsigned nlines;
    uint32_t state;                 /* LEASES_* */
    struct process *owner;          /* nullptr once its handle is gone */
    struct process *grantee;        /* while granted; holds a reference */
    struct process *mapped_in;      /* whose domain maps the registers */
    unsigned refs;
    uint64_t dma_pages;
    struct quarantined *quarantine;
    uint64_t quarantined_pages;
    Handle handle;
    struct lease *next;
};

static spinlock_t lock;
static struct lease *leases;
static atomic_uint live;
static atomic_uint_fast64_t quarantined_total;
static char root;                   /* the device root's object */

unsigned lease_count(void)
{
    return atomic_load(&live);
}

uint64_t lease_quarantined(void)
{
    return atomic_load(&quarantined_total);
}

Handle lease_root(struct process *p)
{
    return handle_new(HANDLE_DEVROOT, &root, p);
}

/* --- Lookups and lifetime, lock held. --- */

static struct lease *find(Handle h)
{
    for (struct lease *l = leases; l && h; l = l->next)
        if (l->handle == h)
            return l;
    return nullptr;
}

/* Drop a reference; true if the caller should kfree it (after unlocking). */
static bool put(struct lease *l)
{
    struct lease **pp;

    if (--l->refs)
        return false;
    for (pp = &leases; *pp != l; pp = &(*pp)->next)
        ;
    *pp = l->next;
    atomic_fetch_sub(&live, 1);
    return true;
}

static void unmap_from(struct lease *l)
{
    area_unmap_phys(l->mapped_in, l->base, l->size);
    l->mapped_in = nullptr;
}

/* Unbind 'ln', masking it; its source goes in 'detach' for the caller
 * to irq_detach once the lock is let go (the handler takes it). */
static void unbind(struct line *ln, uint32_t *detach, unsigned *n)
{
    if (ln->bound) {
        intc_set(ln->source, 0);
        detach[(*n)++] = ln->source;
    }
    ln->bound = false;
    ln->masked = false;
    ln->task = nullptr;
}

/* Lines detached at a time, unlocked, between passes over the leases. */
constexpr unsigned DETACH = 16;

static void detach_all(const uint32_t *detach, unsigned n)
{
    while (n)
        irq_detach(detach[--n]);
}

/* --- What the process code tells us. --- */

void lease_process_dying(struct process *p)
{
    unsigned long was = spin_lock_irqsave(&lock);

    for (struct lease *l = leases; l; l = l->next) {
        if (l->grantee != p)
            continue;
        for (unsigned i = 0; i < l->nlines; i++)
            if (l->line[i].bound) {
                intc_set(l->line[i].source, 0);
                l->line[i].masked = true;
            }
    }
    spin_unlock_irqrestore(&lock, was);
}

/*
 * Two passes: the lines are detached between them, unlocked, so that no
 * handler still runs for a lease when its grant's reference goes.
 */
void lease_process_died(struct process *p)
{
    uint32_t detach[DETACH];
    unsigned n;

    for (bool again = true; again;) {
        unsigned long was = spin_lock_irqsave(&lock);

        n = 0;
        again = false;
        p->leases_closed = true;    /* nothing is granted to it from now on */
        for (struct lease *l = leases; l; l = l->next) {
            if (l->grantee != p)
                continue;
            for (unsigned i = 0; i < l->nlines; i++) {
                if (l->line[i].bound && n == DETACH) {
                    again = true;   /* the rest next time round */
                    break;
                }
                unbind(&l->line[i], detach, &n);
            }
        }
        spin_unlock_irqrestore(&lock, was);
        detach_all(detach, n);
    }
    unsigned long was = spin_lock_irqsave(&lock);
    for (struct lease *l = leases, *next; l; l = next) {
        next = l->next;
        if (l->grantee != p)
            continue;
        if (l->mapped_in == p)
            l->mapped_in = nullptr; /* its domain goes with it */
        l->grantee = nullptr;
        l->state = LEASES_STOPPING;
        process_put(p);
        if (put(l))
            kfree(l);
    }
    spin_unlock_irqrestore(&lock, was);
}

void lease_task_gone(struct task *t)
{
    uint32_t detach[DETACH];
    unsigned n;

    do {
        unsigned long was = spin_lock_irqsave(&lock);

        n = 0;
        for (struct lease *l = leases; l; l = l->next)
            for (unsigned i = 0; i < l->nlines && n < DETACH; i++)
                if (l->line[i].bound && l->line[i].task == t)
                    unbind(&l->line[i], detach, &n);
        spin_unlock_irqrestore(&lock, was);
        detach_all(detach, n);
    } while (n == DETACH);
}

void lease_release(struct lease *l)
{
    unsigned long was = spin_lock_irqsave(&lock);
    bool last;

    if (l->mapped_in && l->mapped_in == l->owner)
        unmap_from(l);
    if (l->quarantine)
        kprintf("planar: lease %p: its owner is gone; %llu pages stay quarantined\n",
                (void *)l->base, (unsigned long long)l->quarantined_pages);
    l->owner = nullptr;
    l->handle = 0;
    last = put(l);
    spin_unlock_irqrestore(&lock, was);
    if (last)
        kfree(l);
}

/* --- DMA Areas. --- */

void lease_dma_new(struct lease *l, uint64_t pages)
{
    unsigned long was = spin_lock_irqsave(&lock);

    l->refs++;
    l->dma_pages += pages;
    spin_unlock_irqrestore(&lock, was);
}

bool lease_dma_gone(struct lease *l, struct area *a, uint64_t pages, bool freed)
{
    struct quarantined *q = nullptr;
    bool last;

    /* Allocated first: the lock is taken with interrupts off. */
    if (!freed)
        q = kmalloc(sizeof *q);
    unsigned long was = spin_lock_irqsave(&lock);
    l->dma_pages -= pages;
    if (!freed && l->state != LEASES_IDLE) {
        /* Its grantee died with it: the device may still write to it. */
        if (q) {
            *q = (struct quarantined){ .area = a, .pages = pages, .next = l->quarantine };
            l->quarantine = q;
        } else {
            /* Nowhere to keep it: leak it, and let the lease go free of it. */
            kprintf("planar: lease %p: an Area of %llu pages leaked, not quarantined\n",
                    (void *)l->base, (unsigned long long)pages);
            last = put(l);
            spin_unlock_irqrestore(&lock, was);
            if (last)
                kfree(l);
            return true;
        }
        l->quarantined_pages += pages;
        atomic_fetch_add(&quarantined_total, pages);
        if (!l->owner)
            kprintf("planar: lease %p: no owner to stop it; %llu pages stay quarantined\n",
                    (void *)l->base, (unsigned long long)pages);
        spin_unlock_irqrestore(&lock, was);
        return true;
    }
    last = put(l);
    spin_unlock_irqrestore(&lock, was);
    kfree(q);
    if (last)
        kfree(l);
    return false;
}

/* --- Interrupts. --- */

static void line_fire(void *data)
{
    struct line *ln = data;
    unsigned long was = spin_lock_irqsave(&lock);

    intc_set(ln->source, 0);
    if (ln->bound) {
        ln->masked = true;
        task_signal(ln->task, ln->sig);
    }
    spin_unlock_irqrestore(&lock, was);
}

/* --- The kernel calls. --- */

static bool overlaps_memory(uint64_t base, uint64_t size)
{
    uint64_t b, s;

    for (unsigned i = 0; memmap_ram(i, &b, &s); i++)
        if (base < b + s && base + size > b)
            return true;
    for (unsigned i = 0; memmap_reserved(i, &b, &s); i++)
        if (base < b + s && base + size > b)
            return true;
    return false;
}

int64_t lease_create_call(uint32_t root_h, uint64_t base, uint64_t size, uint64_t irqs,
                          uint32_t nirqs)
{
    struct process *p = task_self()->process;
    uint32_t src[LEASE_IRQS];
    struct lease *l;
    int64_t err = 0;
    Handle h;

    if (!handle_get(root_h, HANDLE_DEVROOT, p))
        return ERR_BADHANDLE;
    if (size == 0 || size > LEASE_MAX || (base | size) & (PAGE_SIZE - 1) ||
        base + size < base || base + size > ARCH_USER_TOP || nirqs > LEASE_IRQS)
        return ERR_BADARG;
    if (nirqs && !arch_copy_from_user(src, irqs, nirqs * sizeof src[0]))
        return ERR_BADADDR;
    for (unsigned i = 0; i < nirqs; i++) {
        if (src[i] == 0 || src[i] >= IRQ_SOURCES)
            return ERR_BADARG;
        for (unsigned j = 0; j < i; j++)
            if (src[j] == src[i])
                return ERR_BADARG;
    }
    if (overlaps_memory(base, size) || devmem_kernel(base, size))
        return ERR_RIGHTS;
    if (!(l = kmalloc(sizeof *l)))
        return ERR_NOMEM;
    *l = (struct lease){ .base = base, .size = size, .nlines = nirqs, .state = LEASES_IDLE,
                         .owner = p, .refs = 1 };
    for (unsigned i = 0; i < nirqs; i++)
        l->line[i].source = src[i];

    unsigned long was = spin_lock_irqsave(&lock);
    for (struct lease *o = leases; o && !err; o = o->next) {
        if (base < o->base + o->size && base + size > o->base)
            err = ERR_EXISTS;
        for (unsigned i = 0; i < o->nlines; i++)
            for (unsigned j = 0; j < nirqs; j++)
                if (o->line[i].source == src[j])
                    err = ERR_EXISTS;
    }
    /* Another lease's line is ERR_EXISTS, bound or not; then the kernel's. */
    for (unsigned j = 0; j < nirqs && !err; j++)
        if (irq_attached(src[j]))
            err = ERR_RIGHTS;
    if (!err) {
        l->next = leases;
        leases = l;
        atomic_fetch_add(&live, 1);
    }
    spin_unlock_irqrestore(&lock, was);
    if (err) {
        kfree(l);
        return err;
    }
    if (!(h = handle_new(HANDLE_LEASE, l, p))) {
        err = handle_error(p);
        was = spin_lock_irqsave(&lock);
        put(l);
        spin_unlock_irqrestore(&lock, was);
        kfree(l);
        return err;
    }
    was = spin_lock_irqsave(&lock);
    l->handle = h;
    spin_unlock_irqrestore(&lock, was);
    return h;
}

int64_t lease_grant_call(uint32_t h, uint32_t process)
{
    struct process *p = task_self()->process, *child = process_child(process);
    struct lease *l;
    int64_t err = 0;

    if (!child)
        return ERR_BADHANDLE;
    unsigned long was = spin_lock_irqsave(&lock);
    if (!(l = find(h)) || l->owner != p)
        err = ERR_BADHANDLE;
    else if (l->state != LEASES_IDLE)
        err = ERR_BUSY;
    else if (child->leases_closed || atomic_load(&child->dying))
        err = ERR_NOTFOUND;
    if (!err) {
        if (l->mapped_in)
            unmap_from(l);          /* the owner's, while it stopped the device */
        l->grantee = child;         /* keeps our reference */
        l->state = LEASES_GRANTED;
        l->refs++;
    }
    spin_unlock_irqrestore(&lock, was);
    if (err)
        process_put(child);
    return err;
}

int64_t lease_revoke_call(uint32_t h)
{
    struct process *p = task_self()->process;
    struct quarantined *q = nullptr;
    struct lease *l;
    int64_t err = 0;
    unsigned n = 0;

    unsigned long was = spin_lock_irqsave(&lock);
    if (!(l = find(h)) || l->owner != p)
        err = ERR_BADHANDLE;
    else if (l->state == LEASES_GRANTED)
        err = ERR_BUSY;             /* unbind is kill: end the grantee first */
    if (!err) {
        q = l->quarantine;
        l->quarantine = nullptr;
        atomic_fetch_sub(&quarantined_total, l->quarantined_pages);
        l->quarantined_pages = 0;
        l->state = LEASES_IDLE;
        l->refs++;                  /* until its Areas are gone */
    }
    spin_unlock_irqrestore(&lock, was);
    if (err)
        return err;
    /* The device is stopped: its memory can go back. */
    while (q) {
        struct quarantined *next = q->next;

        area_unquarantine(q->area);
        kfree(q);
        q = next;
        n++;
    }
    was = spin_lock_irqsave(&lock);
    l->refs -= n;                   /* the owner's reference and ours remain */
    put(l);
    spin_unlock_irqrestore(&lock, was);
    return 0;
}

int64_t lease_query_call(uint32_t h, uint64_t info_out)
{
    struct process *p = task_self()->process;
    struct LeaseInfo info;
    struct lease *l;
    bool ok;

    unsigned long was = spin_lock_irqsave(&lock);
    if ((ok = (l = find(h)) && (l->owner == p || l->grantee == p)))
        info = (struct LeaseInfo){ .li_Base = l->base, .li_Size = l->size,
                                   .li_State = l->state, .li_Irqs = l->nlines,
                                   .li_DmaPages = l->dma_pages,
                                   .li_QuarantinedPages = l->quarantined_pages };
    spin_unlock_irqrestore(&lock, was);
    if (!ok)
        return ERR_BADHANDLE;
    return arch_copy_to_user(info_out, &info, sizeof info) ? 0 : ERR_BADADDR;
}

int64_t lease_close_call(uint32_t h)
{
    struct process *p = task_self()->process;
    struct lease *l;
    int64_t err = 0;

    /* Checked and taken under the lock, so no GrantLease gets between. */
    unsigned long was = spin_lock_irqsave(&lock);
    if (!(l = find(h)) || l->owner != p)
        err = ERR_BADHANDLE;
    else if (l->state != LEASES_IDLE)
        err = ERR_BUSY;
    else if (handle_take(h, HANDLE_LEASE, p) != l)
        err = ERR_BADHANDLE;
    else
        l->handle = 0;              /* found no more */
    spin_unlock_irqrestore(&lock, was);
    if (err)
        return err;
    lease_release(l);
    return 0;
}

int64_t lease_map_call(uint32_t h, uint64_t addr_out)
{
    struct process *p = task_self()->process;
    struct lease *l;
    int64_t err = 0;
    uint64_t base = 0;

    unsigned long was = spin_lock_irqsave(&lock);
    if (!(l = find(h)) || !(l->state == LEASES_GRANTED ? l->grantee == p : l->owner == p))
        err = ERR_BADHANDLE;
    else if (l->mapped_in)
        err = ERR_EXISTS;
    else if (!area_map_phys(p, l->base, l->size, MAP_USER | MAP_READ | MAP_WRITE | MAP_DEVICE))
        err = ERR_NOMEM;
    if (!err) {
        l->mapped_in = p;
        base = l->base;
    }
    spin_unlock_irqrestore(&lock, was);
    if (err)
        return err;
    if (arch_copy_to_user(addr_out, &base, sizeof base))
        return 0;
    was = spin_lock_irqsave(&lock);
    if (find(h) == l && l->mapped_in == p)
        unmap_from(l);              /* so a retry can map it */
    spin_unlock_irqrestore(&lock, was);
    return ERR_BADADDR;
}

/* The line 'line' of the lease 'h', if granted to the calling process;
 * else nullptr, and why in 'err'. Lock held. */
static struct line *grantees_line(Handle h, uint32_t line, int64_t *err)
{
    struct process *p = task_self()->process;
    struct lease *l = find(h);

    if (!l || l->state != LEASES_GRANTED || l->grantee != p) {
        *err = ERR_BADHANDLE;
        return nullptr;
    }
    if (line >= l->nlines) {
        *err = ERR_BADARG;
        return nullptr;
    }
    return &l->line[line];
}

int64_t lease_bind_call(uint32_t h, uint32_t line, int64_t sigbit)
{
    struct line *ln;
    int64_t err = 0;

    if (!syscall_sigbit(sigbit))
        return ERR_BADARG;
    unsigned long was = spin_lock_irqsave(&lock);
    if (!(ln = grantees_line(h, line, &err)))
        ;
    else if (ln->bound)
        err = ERR_EXISTS;
    if (!err) {
        ln->task = task_self();
        ln->sig = 1ULL << sigbit;
        ln->bound = true;
        ln->masked = false;
        if (!irq_attach(ln->source, IRQ_LEVEL, line_fire, ln)) {
            ln->bound = false;
            ln->task = nullptr;
            err = ERR_BADARG;
        }
    }
    spin_unlock_irqrestore(&lock, was);
    return err;
}

uint64_t lease_ack_wait_call(uint32_t h, uint32_t line, uint64_t mask)
{
    struct task *self = task_self();
    struct line *ln;
    int64_t err = ERR_BADHANDLE;
    bool ok;

    unsigned long was = spin_lock_irqsave(&lock);
    ok = (ln = grantees_line(h, line, &err)) && ln->bound && ln->task == self &&
         !atomic_load(&self->process->dying);
    if (ok && ln->masked) {
        ln->masked = false;
        intc_set(ln->source, IRQ_LEVEL);
    }
    spin_unlock_irqrestore(&lock, was);
    if (!ok)
        return mask ? 0 : (uint64_t)err;
    return mask ? task_wait(mask) : 0;
}

int64_t lease_dma_call(uint32_t h, uint64_t size, uint32_t flags, uint64_t addr_out)
{
    struct process *p = task_self()->process;
    struct lease *l;
    int64_t r;
    bool last;

    unsigned long was = spin_lock_irqsave(&lock);
    if ((l = find(h)) && l->state == LEASES_GRANTED && l->grantee == p)
        l->refs++;                  /* until the Area holds its own */
    else
        l = nullptr;
    spin_unlock_irqrestore(&lock, was);
    if (!l)
        return ERR_BADHANDLE;
    r = area_alloc_dma(l, size, flags, addr_out);
    was = spin_lock_irqsave(&lock);
    last = put(l);
    spin_unlock_irqrestore(&lock, was);
    if (last)
        kfree(l);
    return r;
}

/* The DTB, or the ACPI tables, mapped read-only where they are, into p. */
static int64_t map_firmware(struct process *p, struct FirmwareInfo *fi)
{
    uint64_t pa, len;

    *fi = (struct FirmwareInfo){ 0 };
    if (fdt_blob()) {
        fi->fi_Dtb = virt_to_phys(fdt_blob());
        fi->fi_DtbSize = fdt_size();
        pa = fi->fi_Dtb & ~(PAGE_SIZE - 1);
        len = (fi->fi_Dtb + fi->fi_DtbSize - pa + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (!area_map_phys(p, pa, len, MAP_USER | MAP_READ))
            return ERR_NOMEM;
    }
    for (unsigned i = 0; acpi_blob(i, &pa, &len); i++) {
        uint64_t page = pa & ~(PAGE_SIZE - 1);

        if (i == 0)
            fi->fi_Rsdp = pa;
        if (!area_map_phys(p, page, (pa + len - page + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                           MAP_USER | MAP_READ))
            return ERR_NOMEM;
    }
    return 0;
}

int64_t lease_firmware_call(uint32_t root_h, uint64_t info_out)
{
    struct process *p = task_self()->process;
    struct FirmwareInfo fi;
    int64_t err;

    if (!handle_get(root_h, HANDLE_DEVROOT, p))
        return ERR_BADHANDLE;
    if ((err = map_firmware(p, &fi)))
        return err;
    return arch_copy_to_user(info_out, &fi, sizeof fi) ? 0 : ERR_BADADDR;
}

/* The same, into a process the root's holder made: acpid (step 4). */
int64_t lease_firmware_grant_call(uint32_t root_h, uint32_t process)
{
    struct process *p = task_self()->process, *child;
    struct FirmwareInfo fi;
    int64_t err;

    if (!handle_get(root_h, HANDLE_DEVROOT, p) || !(child = process_child(process)))
        return ERR_BADHANDLE;
    err = atomic_load(&child->dying) ? ERR_NOTFOUND : map_firmware(child, &fi);
    process_put(child);
    return err;
}
