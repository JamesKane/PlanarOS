/*
 * kernel/va.c -- the global layout's address allocator
 *
 * A sorted list of free extents, first fit, merged with their neighbours
 * when ranges come back. Areas are few and long-lived next to pages, so
 * a list is enough for now.
 */
#include "kmalloc.h"
#include "pmm.h"
#include "spinlock.h"
#include "va.h"

struct extent {
    uint64_t start, end;
    struct extent *next;
};

static struct extent *free_list;
static bool ready;
static uint64_t free_bytes;
static spinlock_t lock;

static uint64_t round_up(uint64_t v, uint64_t to)
{
    return (v + to - 1) & ~(to - 1);
}

/* The whole window is free until the first allocation. Lock held. */
static void init(struct extent *e)
{
    ready = true;
    if (!e)
        return;
    *e = (struct extent){ .start = VA_WINDOW_BASE, .end = VA_WINDOW_TOP };
    free_list = e;
    free_bytes = e->end - e->start;
}

uint64_t va_alloc(uint64_t bytes, uint64_t align)
{
    struct extent **pp, *e, *spare, *first;
    uint64_t va = 0, base, end;

    if (!bytes || bytes > VA_WINDOW_TOP - VA_WINDOW_BASE || align < PAGE_SIZE ||
        (align & (align - 1)))
        return 0;
    bytes = round_up(bytes, PAGE_SIZE);
    /* Splitting an extent in the middle needs a second record; get it
     * (and the first-use one) before taking the list. */
    spare = kmalloc(sizeof *spare);
    first = ready ? nullptr : kmalloc(sizeof *first);
    unsigned long was = spin_lock_irqsave(&lock);
    if (!ready) {
        init(first);
        first = nullptr;
    }
    for (pp = &free_list; (e = *pp); pp = &e->next) {
        va = round_up(e->start + PAGE_SIZE, align);
        base = va - PAGE_SIZE;
        end = va + bytes;
        if (va < e->start || end > e->end || end < va)
            continue;
        if (base > e->start && end < e->end) {
            if (!spare)
                continue;
            *spare = (struct extent){ .start = end, .end = e->end, .next = e->next };
            e->end = base;
            e->next = spare;
            spare = nullptr;
        } else if (base > e->start) {
            e->end = base;
        } else if (end < e->end) {
            e->start = end;
        } else {
            *pp = e->next;
            kfree(e);
        }
        free_bytes -= end - base;
        break;
    }
    spin_unlock_irqrestore(&lock, was);
    kfree(spare);
    kfree(first);
    return e ? va : 0;
}

void va_free(uint64_t start, uint64_t bytes)
{
    struct extent **pp, *e, *prev = nullptr, *n;
    uint64_t end;

    start &= ~(PAGE_SIZE - 1);
    end = round_up(start + bytes, PAGE_SIZE);
    if (end <= start)
        return;
    n = kmalloc(sizeof *n);
    unsigned long was = spin_lock_irqsave(&lock);
    for (pp = &free_list; (e = *pp) && e->start < start; pp = &e->next)
        prev = e;
    free_bytes += end - start;
    if (prev && prev->end == start) {
        prev->end = end;
        if (e && e->start == end) {         /* closes a gap: three become one */
            prev->end = e->end;
            prev->next = e->next;
            kfree(e);
        }
    } else if (e && e->start == end) {
        e->start = start;
    } else if (n) {
        *n = (struct extent){ .start = start, .end = end, .next = e };
        *pp = n;
        n = nullptr;
    } else {
        free_bytes -= end - start;          /* no memory to record it: lost */
    }
    spin_unlock_irqrestore(&lock, was);
    kfree(n);
}

uint64_t va_free_bytes(void)
{
    unsigned long was = spin_lock_irqsave(&lock);
    uint64_t n = ready ? free_bytes : VA_WINDOW_TOP - VA_WINDOW_BASE;

    spin_unlock_irqrestore(&lock, was);
    return n;
}
