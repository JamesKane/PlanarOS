/*
 * kernel/futex.c -- waiting on a word in memory
 *
 * docs §4.1 and v5-plan step 1. A waiter goes on its word's bucket
 * before it reads the word, and a waker changes the word before it
 * takes the bucket's lock, so a wake can't fall between the read and
 * the sleep: a waker that comes after the read finds the waiter listed.
 * Words are keyed by physical address, which under the identity layout
 * is the address every process maps them at, and is looked up in the
 * caller's own domain, so a task can only wait on, or wake, memory it
 * maps.
 *
 * A waiter is woken by SIGF_WAKE, which only this file sends, and only
 * to a task on a bucket's list, under that bucket's lock. So once a
 * waiter is off its list no wake is on its way, and clearing the bit
 * then leaves nothing behind for its next wait.
 */
#include <planar/syscalls.h>

#include "arch.h"
#include "futex.h"
#include "ktime.h"
#include "pmm.h"
#include "sched.h"
#include "spinlock.h"

constexpr unsigned BUCKETS = 64;

struct waiter {
    uint64_t key;                   /* the word's physical address */
    struct task *task;
    bool woken;                     /* by WakeAddr, not the deadline */
    struct waiter *next;
};

static struct bucket {
    spinlock_t lock;
    struct waiter *list;
} buckets[BUCKETS];

static atomic_uint waiting;

unsigned futex_waiters(void)
{
    return atomic_load(&waiting);
}

static struct bucket *bucket_of(uint64_t key)
{
    return &buckets[(key >> 3 ^ key >> 12) % BUCKETS];
}

/* The physical address of the word at 'addr' in the caller's domain, or
 * 0 if it isn't 8-byte aligned or mapped. */
static uint64_t key_of(uint64_t addr)
{
    struct task *self = task_self();
    uint64_t pa;

    if (!self->domain || (addr & 7) || addr >= ARCH_USER_TOP)
        return 0;
    if (!(pa = arch_lookup(self->domain, addr & ~(PAGE_SIZE - 1))))
        return 0;
    return pa | (addr & (PAGE_SIZE - 1));
}

static void unlist(struct bucket *b, struct waiter *w)
{
    for (struct waiter **pp = &b->list; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            atomic_fetch_sub(&waiting, 1);
            return;
        }
}

static void deadline_fire(void *data)
{
    task_signal(data, SIGF_WAKE);
}

int64_t futex_wait_call(uint64_t addr, uint64_t value, uint64_t deadline)
{
    struct task *self = task_self();
    struct waiter w = { .key = key_of(addr), .task = self };
    struct bucket *b = bucket_of(w.key);
    uint64_t now;
    bool woken, read;

    if (!w.key)
        return ERR_BADADDR;
    if (deadline && !time_have_timers())
        return ERR_NOSYS;
    unsigned long was = spin_lock_irqsave(&b->lock);
    w.next = b->list;
    b->list = &w;
    atomic_fetch_add(&waiting, 1);
    spin_unlock_irqrestore(&b->lock, was);

    uint64_t word;

    /* Through the caller's own mapping: the word may be in memory the CPU
     * doesn't cache (AREAF_NC), which the kernel's direct map does. */
    read = arch_copy_from_user(&word, addr, sizeof word);
    if (read && word == value) {
        if (deadline) {
            self->sleep = (struct ktimer){ .fn = deadline_fire, .data = self };
            ktimer_start(&self->sleep, time_from_ns(deadline));
        }
        task_wait(SIGF_WAKE);       /* or killed: back out the same way */
        if (deadline)
            ktimer_stop(&self->sleep);
    }
    was = spin_lock_irqsave(&b->lock);
    if (!(woken = w.woken))
        unlist(b, &w);              /* a waker takes it off the list itself */
    spin_unlock_irqrestore(&b->lock, was);
    task_set_signal(0, SIGF_WAKE);
    if (!read)
        return ERR_BADADDR;
    now = time_ns(time_now());
    return !woken && word == value && deadline && now >= deadline ? ERR_TIMEOUT : 0;
}

void futex_wake_range(uint64_t pa, uint64_t size)
{
    for (unsigned i = 0; i < BUCKETS; i++) {
        struct bucket *b = &buckets[i];
        unsigned long was = spin_lock_irqsave(&b->lock);

        for (struct waiter **pp = &b->list; *pp;) {
            struct waiter *w = *pp;

            if (w->key - pa >= size) {
                pp = &w->next;
                continue;
            }
            *pp = w->next;
            atomic_fetch_sub(&waiting, 1);
            w->woken = true;
            task_signal(w->task, SIGF_WAKE);
        }
        spin_unlock_irqrestore(&b->lock, was);
    }
}

int64_t futex_wake_call(uint64_t addr, uint32_t count)
{
    uint64_t key = key_of(addr);
    struct bucket *b = bucket_of(key);
    int64_t n = 0;

    if (!key)
        return ERR_BADADDR;
    unsigned long was = spin_lock_irqsave(&b->lock);
    for (struct waiter **pp = &b->list; *pp && n < count;) {
        struct waiter *w = *pp;

        if (w->key != key) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        atomic_fetch_sub(&waiting, 1);
        w->woken = true;
        task_signal(w->task, SIGF_WAKE);
        n++;
    }
    spin_unlock_irqrestore(&b->lock, was);
    return n;
}
