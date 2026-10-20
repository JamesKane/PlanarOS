/*
 * kernel/port.c -- ports and connections
 *
 * One lock covers every port and connection. Nothing here sleeps. The
 * tasks that ports and ends point at stay valid while the lock is held,
 * because a task leaving its process first detaches from everything here
 * (ipc_task_gone), under the same lock. Signals are posted under the
 * lock and the CPUs they call for are kicked once it's let go.
 */
#include <planar/ipc.h>
#include <planar/syscalls.h>

#include "area.h"
#include "cpu.h"
#include "handle.h"
#include "kmalloc.h"
#include "kstring.h"
#include "pmm.h"
#include "port.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"

constexpr unsigned PORT_NAME = 32;
constexpr unsigned PORT_QUEUE = 32;     /* connections waiting to be accepted */

enum side { CLIENT, SERVER };

struct conn;

struct end {
    struct conn *conn;
    bool open;
    struct task *task;              /* signalled; nullptr unless open */
    struct process *proc;
    uint64_t sig;
    Handle handle;
};

struct conn {
    struct end end[2];
    struct area *arena;
    struct port *port;              /* queued there, not yet accepted */
    struct conn *next_queued, *next;
};

struct port {
    char name[PORT_NAME];
    struct task *task;
    struct process *proc;
    uint64_t sig;
    Handle handle;
    struct conn *queue;             /* oldest first */
    unsigned queued;
    struct port *next;
};

static spinlock_t lock;
static struct port *ports;
static struct conn *conns;
static atomic_uint live;

unsigned ipc_count(void)
{
    return atomic_load(&live);
}

/* --- Signals, kicked after the lock. --- */

static void post(struct task *t, uint64_t sig, uint64_t *kicks)
{
    struct cpu *c = task_post(t, sig, 0);

    if (c)
        *kicks |= 1ULL << c->index;
}

static void kick_all(uint64_t kicks)
{
    for (unsigned i = 0; kicks; i++, kicks >>= 1)
        if (kicks & 1)
            sched_kick(&cpus[i]);
}

/* --- Lookups, lock held. --- */

static struct port *find_port(const char *name)
{
    for (struct port *pt = ports; pt; pt = pt->next)
        if (kstreq(pt->name, name))
            return pt;
    return nullptr;
}

static struct end *end_of(Handle h)
{
    return handle_get(h, HANDLE_CONN, task_self()->process);
}

static struct ConnArena *header(const struct conn *c)
{
    return phys_to_virt(area_pa(c->arena, 0));
}

/* --- Closing, lock held. --- */

static void unqueue(struct conn *c)
{
    struct conn **pp;

    c->port->queued--;
    for (pp = &c->port->queue; *pp != c; pp = &(*pp)->next_queued)
        ;
    *pp = c->next_queued;
    c->port = nullptr;
}

static void free_conn(struct conn *c)
{
    struct conn **pp;

    for (pp = &conns; *pp != c; pp = &(*pp)->next)
        ;
    *pp = c->next;
    kfree(c);
    atomic_fetch_sub(&live, 1);
}

/* Close one end: say so in the arena, tell the other end, and unmap
 * the arena from this end's process (after writing to it: the other
 * end may have gone already). */
static void close_end(struct end *e, uint64_t *kicks)
{
    struct conn *c = e->conn;
    struct end *peer = &c->end[e == &c->end[CLIENT] ? SERVER : CLIENT];

    atomic_fetch_or(&header(c)->ca_Closed,
                    e == &c->end[CLIENT] ? CONNF_CLIENT_GONE : CONNF_SERVER_GONE);
    if (peer->open)
        post(peer->task, peer->sig, kicks);
    handle_free(e->handle);
    e->proc->conns--;
    e->open = false;
    e->task = nullptr;
    area_release(c->arena, e->proc);
    if (c->port)
        unqueue(c);
    if (!peer->open && !c->port)
        free_conn(c);
}

/* Drop a port: its name goes, and connections it never accepted are
 * told the server has gone. */
static void drop_port(struct port *pt, uint64_t *kicks)
{
    struct port **pp;

    for (pp = &ports; *pp != pt; pp = &(*pp)->next)
        ;
    *pp = pt->next;
    handle_free(pt->handle);
    while (pt->queue) {
        struct conn *c = pt->queue;

        atomic_fetch_or(&header(c)->ca_Closed, CONNF_SERVER_GONE);
        post(c->end[CLIENT].task, c->end[CLIENT].sig, kicks);
        unqueue(c);
    }
    kfree(pt);
    atomic_fetch_sub(&live, 1);
}

void ipc_task_gone(struct task *t)
{
    uint64_t kicks = 0;
    unsigned long was = spin_lock_irqsave(&lock);

    for (struct conn *c = conns, *next; c; c = next) {
        next = c->next;
        /* Closing both ends frees the connection: look no further. */
        if (c->end[CLIENT].open && c->end[CLIENT].task == t) {
            bool last = !c->end[SERVER].open && !c->port;

            close_end(&c->end[CLIENT], &kicks);
            if (last)
                continue;
        }
        if (c->end[SERVER].open && c->end[SERVER].task == t)
            close_end(&c->end[SERVER], &kicks);
    }
    for (struct port *pt = ports, *next; pt; pt = next) {
        next = pt->next;
        if (pt->task == t)
            drop_port(pt, &kicks);
    }
    spin_unlock_irqrestore(&lock, was);
    kick_all(kicks);
}

bool ipc_connected(const struct process *a, const struct process *b)
{
    unsigned long was = spin_lock_irqsave(&lock);
    bool found = false;

    for (struct conn *c = conns; c && !found; c = c->next) {
        const struct end *x = &c->end[CLIENT], *y = &c->end[SERVER];

        found = x->open && y->open &&
                ((x->proc == a && y->proc == b) || (x->proc == b && y->proc == a));
    }
    spin_unlock_irqrestore(&lock, was);
    return found;
}

/* --- The kernel calls. --- */

static bool user_sigbit(int64_t bit)
{
    return bit >= 16 && bit < 64;
}

int64_t port_create_call(uint64_t user_name, int64_t sigbit)
{
    struct task *self = task_self();
    struct port *pt;
    unsigned long was;

    if (!user_sigbit(sigbit))
        return ERR_BADARG;
    if (!(pt = kmalloc(sizeof *pt)))
        return ERR_NOMEM;
    *pt = (struct port){ .task = self, .proc = self->process, .sig = 1ULL << sigbit };
    if (!syscall_copy_string(pt->name, user_name, sizeof pt->name) || !pt->name[0]) {
        kfree(pt);
        return ERR_BADADDR;
    }
    was = spin_lock_irqsave(&lock);
    if (find_port(pt->name)) {
        spin_unlock_irqrestore(&lock, was);
        kfree(pt);
        return ERR_EXISTS;
    }
    if (!(pt->handle = handle_new(HANDLE_PORT, pt, self->process))) {
        spin_unlock_irqrestore(&lock, was);
        kfree(pt);
        return handle_error(self->process);
    }
    pt->next = ports;
    ports = pt;
    atomic_fetch_add(&live, 1);
    spin_unlock_irqrestore(&lock, was);
    return pt->handle;
}

int64_t port_delete_call(uint32_t h)
{
    uint64_t kicks = 0;
    unsigned long was = spin_lock_irqsave(&lock);
    struct port *pt = handle_get(h, HANDLE_PORT, task_self()->process);

    if (pt)
        drop_port(pt, &kicks);
    spin_unlock_irqrestore(&lock, was);
    kick_all(kicks);
    return pt ? 0 : ERR_BADHANDLE;
}

/* The arena: header, request ring, reply ring, heap, each 64-byte aligned. */
static uint64_t ring_bytes(uint32_t slots)
{
    return (sizeof(struct ConnRing) + slots * sizeof(uint64_t) + 63) & ~63ULL;
}

int64_t port_open_call(uint64_t user_name, int64_t sigbit, uint64_t slots, uint64_t heap,
                       uint64_t arena_out)
{
    struct task *self = task_self();
    struct process *p = self->process;
    char name[PORT_NAME];
    struct ConnArena hdr;
    struct area *arena;
    struct conn *c;
    struct port *pt;
    uint64_t kicks = 0, va = 0;
    int64_t err = 0;
    unsigned long was;

    if (!slots)
        slots = CONN_SLOTS_DEFAULT;
    if (!heap)
        heap = CONN_HEAP_DEFAULT;
    if (!user_sigbit(sigbit) || slots < 2 || slots > CONN_SLOTS_MAX || (slots & (slots - 1)) ||
        heap > CONN_HEAP_MAX)
        return ERR_BADARG;
    if (!syscall_copy_string(name, user_name, sizeof name) ||
        !arch_copy_to_user(arena_out, &va, sizeof va))
        return ERR_BADADDR;

    hdr = (struct ConnArena){ .ca_Magic = CONN_MAGIC, .ca_Version = CONN_VERSION,
                              .ca_Slots = (uint32_t)slots, .ca_Request = sizeof hdr };
    hdr.ca_Reply = hdr.ca_Request + ring_bytes((uint32_t)slots);
    hdr.ca_Heap = hdr.ca_Reply + ring_bytes((uint32_t)slots);
    hdr.ca_Size = (hdr.ca_Heap + heap + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    hdr.ca_HeapSize = hdr.ca_Size - hdr.ca_Heap;
    if (!(c = kmalloc(sizeof *c)))
        return ERR_NOMEM;
    if (!(arena = area_new(layout_boot(), hdr.ca_Size / PAGE_SIZE, 0, false))) {
        kfree(c);
        return ERR_NOMEM;
    }
    area_copy_in(arena, 0, &hdr, sizeof hdr);   /* the rest is zero: empty rings */
    if (!area_map(arena, p, AREAR_READ | AREAR_WRITE, 0)) {
        area_destroy(arena);
        kfree(c);
        return ERR_NOMEM;
    }
    *c = (struct conn){ .arena = arena };
    c->end[CLIENT] = (struct end){ .conn = c, .open = true, .task = self, .proc = p,
                                   .sig = 1ULL << sigbit };
    c->end[SERVER].conn = c;

    was = spin_lock_irqsave(&lock);
    if (!(pt = find_port(name)))
        err = ERR_NOTFOUND;
    else if (pt->proc == p)
        err = ERR_BADARG;           /* a process needs no connection to itself */
    else if (p->conns >= PROCESS_CONNS)
        err = ERR_LIMIT;
    else if (pt->queued >= PORT_QUEUE)
        err = ERR_FULL;             /* the server isn't accepting: try later */
    else if (!(c->end[CLIENT].handle = handle_new(HANDLE_CONN, &c->end[CLIENT], p)))
        err = handle_error(p);
    if (err) {
        spin_unlock_irqrestore(&lock, was);
        area_release(arena, p);
        kfree(c);
        return err;
    }
    /* Queue it for AcceptConn, oldest first, and tell the server. */
    struct conn **pp;
    for (pp = &pt->queue; *pp; pp = &(*pp)->next_queued)
        ;
    *pp = c;
    c->port = pt;
    pt->queued++;
    p->conns++;
    c->next = conns;
    conns = c;
    atomic_fetch_add(&live, 1);
    post(pt->task, pt->sig, &kicks);
    spin_unlock_irqrestore(&lock, was);
    kick_all(kicks);

    va = area_base(arena);
    if (!arch_copy_to_user(arena_out, &va, sizeof va))
        return ERR_BADADDR;     /* the handle stands; closing it cleans up */
    return c->end[CLIENT].handle;
}

int64_t port_accept_call(uint32_t h, int64_t sigbit, uint64_t arena_out)
{
    struct task *self = task_self();
    struct process *p = self->process;
    struct port *pt;
    struct conn *c;
    struct end *e;
    uint64_t va = 0;
    int64_t err = 0;
    unsigned long was;

    if (!user_sigbit(sigbit))
        return ERR_BADARG;
    if (!arch_copy_to_user(arena_out, &va, sizeof va))
        return ERR_BADADDR;
    was = spin_lock_irqsave(&lock);
    if (!(pt = handle_get(h, HANDLE_PORT, p)))
        err = ERR_BADHANDLE;
    else if (!(c = pt->queue))
        err = ERR_NOTFOUND;
    else if (p->conns >= PROCESS_CONNS)
        err = ERR_LIMIT;
    else if (area_share(c->arena, p, AREAR_READ | AREAR_WRITE) != 0)
        err = ERR_NOMEM;
    if (err) {
        spin_unlock_irqrestore(&lock, was);
        return err;
    }
    e = &c->end[SERVER];
    if (!(e->handle = handle_new(HANDLE_CONN, e, p))) {
        area_release(c->arena, p);
        spin_unlock_irqrestore(&lock, was);
        return handle_error(p);
    }
    unqueue(c);
    p->conns++;
    e->open = true;
    e->task = self;
    e->proc = p;
    e->sig = 1ULL << sigbit;
    va = area_base(c->arena);
    h = e->handle;
    spin_unlock_irqrestore(&lock, was);
    if (!arch_copy_to_user(arena_out, &va, sizeof va))
        return ERR_BADADDR;
    return h;
}

int64_t conn_signal_call(uint32_t h)
{
    uint64_t kicks = 0;
    int64_t err = 0;
    struct end *e, *peer;
    unsigned long was;

    task_end_loan();            /* a reply ends what a DoMsg lent */
    was = spin_lock_irqsave(&lock);
    if (!(e = end_of(h))) {
        err = ERR_BADHANDLE;
    } else {
        peer = &e->conn->end[e == &e->conn->end[CLIENT] ? SERVER : CLIENT];
        if (peer->open)
            post(peer->task, peer->sig, &kicks);
        else if (!e->conn->port)
            err = ERR_PORT_DIED;    /* (queued: the server sees it on accepting) */
    }
    spin_unlock_irqrestore(&lock, was);
    kick_all(kicks);
    return err;
}

uint64_t conn_signal_wait_call(uint32_t h, uint64_t mask)
{
    struct cpu *c = nullptr, *here;
    struct end *e, *peer = nullptr;
    unsigned long was;

    was = spin_lock_irqsave(&lock);
    if ((e = end_of(h))) {
        peer = &e->conn->end[e == &e->conn->end[CLIENT] ? SERVER : CLIENT];
        /* A client waiting on its server lends it priority; a server
         * waking its client on the way to sleep only hands over. */
        if (peer->open)
            c = task_post(peer->task, peer->sig,
                          POST_HANDOFF | (e == &e->conn->end[CLIENT] ? POST_LEND : 0));
        else if (!e->conn->port)
            peer = nullptr;         /* gone: don't wait for it */
    }
    here = this_cpu();
    spin_unlock_irqrestore(&lock, was);
    if (!peer)
        return 0;
    /* Woken here, the peer runs as soon as this task waits. */
    if (c && c != here) {
        sched_kick(c);
        c = nullptr;
    }
    return task_wait_kick(mask, c);
}

int64_t conn_close_call(uint32_t h)
{
    uint64_t kicks = 0;
    unsigned long was = spin_lock_irqsave(&lock);
    struct end *e = end_of(h);

    if (e)
        close_end(e, &kicks);
    spin_unlock_irqrestore(&lock, was);
    kick_all(kicks);
    return e ? 0 : ERR_BADHANDLE;
}
