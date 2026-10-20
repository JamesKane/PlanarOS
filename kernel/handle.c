/*
 * kernel/handle.c -- handles
 */
#include "handle.h"
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

    for (unsigned n = 0; n < HANDLES - 1; n++) {
        unsigned i = 1 + (next - 1 + n) % (HANDLES - 1);

        if (table[i].type == HANDLE_FREE) {
            table[i].type = type;
            table[i].obj = obj;
            table[i].owner = owner;
            if (!table[i].gen)
                table[i].gen = 1;
            next = i + 1;
            used++;
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
    e->type = HANDLE_FREE;
    e->obj = nullptr;
    e->owner = nullptr;
    if (!++e->gen)
        e->gen = 1;
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

void handle_free_owner(struct process *owner)
{
    unsigned long was = spin_lock_irqsave(&lock);

    for (unsigned i = 1; i < HANDLES; i++)
        if (table[i].type != HANDLE_FREE && table[i].owner == owner)
            release(&table[i]);
    spin_unlock_irqrestore(&lock, was);
}

unsigned handle_count(void)
{
    return used;
}
