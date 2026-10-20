/*
 * kernel/handle.c -- handles
 */
#include <planar/syscalls.h>

#include "handle.h"
#include "lease.h"
#include "process.h"
#include "spinlock.h"

constexpr unsigned HANDLES = 4096;  /* index 0 is never handed out */

static struct entry {
    uint16_t gen;
    enum handle_type type;
    void *obj;
    struct process *owner;
} table[HANDLES];

static unsigned next = 1, used;
static spinlock_t lock;

Handle handle_new(enum handle_type type, void *obj, struct process *owner)
{
    unsigned long was = spin_lock_irqsave(&lock);

    if (owner && owner->handles >= PROCESS_HANDLES) {
        spin_unlock_irqrestore(&lock, was);
        return 0;
    }
    for (unsigned n = 0; n < HANDLES - 1; n++) {
        unsigned i = 1 + (next - 1 + n) % (HANDLES - 1);

        if (table[i].type == HANDLE_FREE) {
            __atomic_store_n(&table[i].type, type, __ATOMIC_RELAXED);
            __atomic_store_n(&table[i].obj, obj, __ATOMIC_RELAXED);
            __atomic_store_n(&table[i].owner, owner, __ATOMIC_RELAXED);
            if (!table[i].gen)
                __atomic_store_n(&table[i].gen, 1, __ATOMIC_RELEASE);
            next = i + 1;
            used++;
            if (owner)
                owner->handles++;
            spin_unlock_irqrestore(&lock, was);
            return (Handle)table[i].gen << 16 | i;
        }
    }
    spin_unlock_irqrestore(&lock, was);
    return 0;
}

/* Lock held. */
static struct entry *lookup(Handle h)
{
    unsigned i = h & 0xffff;

    if (i == 0 || i >= HANDLES || table[i].type == HANDLE_FREE ||
        table[i].gen != (uint16_t)(h >> 16))
        return nullptr;
    return &table[i];
}

/* Lock held. */
static void release(struct entry *e)
{
    uint16_t gen = (uint16_t)(e->gen + 1);

    if (e->owner)
        e->owner->handles--;
    __atomic_store_n(&e->gen, gen ? gen : 1, __ATOMIC_RELEASE);    /* first: see handle_get */
    __atomic_store_n(&e->type, HANDLE_FREE, __ATOMIC_RELAXED);
    __atomic_store_n(&e->obj, nullptr, __ATOMIC_RELAXED);
    __atomic_store_n(&e->owner, nullptr, __ATOMIC_RELAXED);
    used--;
}

void handle_free(Handle h)
{
    unsigned long was = spin_lock_irqsave(&lock);
    struct entry *e = lookup(h);

    if (e)
        release(e);
    spin_unlock_irqrestore(&lock, was);
}

bool handle_with(Handle h, enum handle_type type, void (*fn)(void *, void *), void *arg)
{
    unsigned long was = spin_lock_irqsave(&lock);
    struct entry *e = lookup(h);
    bool ran = e && e->type == type;

    if (ran)
        fn(e->obj, arg);
    spin_unlock_irqrestore(&lock, was);
    return ran;
}

/*
 * Without the table's lock: the caller's lock keeps the entry from being
 * freed, and an entry freed earlier has moved on to another generation,
 * so a stale handle can't match whatever is being written there now.
 */
void *handle_get(Handle h, enum handle_type type, const struct process *owner)
{
    unsigned i = h & 0xffff;
    struct entry *e = &table[i];

    if (i == 0 || i >= HANDLES ||
        __atomic_load_n(&e->gen, __ATOMIC_ACQUIRE) != (uint16_t)(h >> 16) ||
        __atomic_load_n(&e->type, __ATOMIC_RELAXED) != type ||
        __atomic_load_n(&e->owner, __ATOMIC_RELAXED) != owner)
        return nullptr;
    return __atomic_load_n(&e->obj, __ATOMIC_RELAXED);
}

void *handle_take(Handle h, enum handle_type type, const struct process *owner)
{
    unsigned long was = spin_lock_irqsave(&lock);
    struct entry *e = lookup(h);
    void *obj = nullptr;

    if (e && e->type == type && e->owner == owner) {
        obj = e->obj;
        release(e);
    }
    spin_unlock_irqrestore(&lock, was);
    return obj;
}

void handle_free_owner(struct process *owner)
{
    for (unsigned i = 1; i < HANDLES;) {
        struct { enum handle_type type; void *obj; } held[16];
        unsigned n = 0;
        unsigned long was = spin_lock_irqsave(&lock);

        for (; i < HANDLES && n < 16; i++) {
            if (table[i].type == HANDLE_FREE || table[i].owner != owner)
                continue;
            if (table[i].type == HANDLE_PROCESS || table[i].type == HANDLE_LEASE) {
                held[n].type = table[i].type;
                held[n++].obj = table[i].obj;
            }
            release(&table[i]);
        }
        spin_unlock_irqrestore(&lock, was);
        for (unsigned k = 0; k < n; k++) {
            if (held[k].type == HANDLE_PROCESS)
                process_release(held[k].obj);
            else
                lease_release(held[k].obj);
        }
    }
}

int64_t handle_error(const struct process *owner)
{
    return owner && __atomic_load_n(&owner->handles, __ATOMIC_RELAXED) >= PROCESS_HANDLES
               ? ERR_LIMIT : ERR_NOMEM;
}

unsigned handle_count(void)
{
    return used;
}
