/*
 * kernel/kmalloc.c -- small kernel objects
 *
 * Each block carries its size class in a 16-byte header; freed blocks go
 * on their class's free list. Pages are never returned to the page
 * allocator: kernel objects come and go in roughly steady numbers.
 */
#include "arch.h"
#include "kmalloc.h"
#include "kstring.h"
#include "pmm.h"
#include "spinlock.h"

constexpr unsigned HEADER = 16;
constexpr unsigned MIN_SHIFT = 5;       /* 32 bytes, header included */
constexpr unsigned MAX_SHIFT = 12;      /* a page: 4,080 bytes of object */
constexpr unsigned CLASSES = MAX_SHIFT - MIN_SHIFT + 1;

struct block {
    struct block *next;                 /* while free */
};

static struct block *free_list[CLASSES];
static uint64_t pages_taken;
static spinlock_t lock;

static unsigned class_of(size_t size)
{
    unsigned c = 0;

    while ((1UL << (MIN_SHIFT + c)) < size + HEADER)
        c++;
    return c;
}

void *kmalloc(size_t size)
{
    unsigned c = class_of(size);
    size_t bytes = 1UL << (MIN_SHIFT + c);
    struct block *b;
    uint8_t *p;

    if (c >= CLASSES)
        return nullptr;
    unsigned long was = spin_lock_irqsave(&lock);
    if (!free_list[c]) {
        uint64_t pa = pmm_alloc();

        if (!pa) {
            spin_unlock_irqrestore(&lock, was);
            return nullptr;
        }
        pages_taken++;
        for (size_t off = 0; off + bytes <= PAGE_SIZE; off += bytes) {
            b = (struct block *)((uint8_t *)phys_to_virt(pa) + off);
            b->next = free_list[c];
            free_list[c] = b;
        }
    }
    b = free_list[c];
    free_list[c] = b->next;
    spin_unlock_irqrestore(&lock, was);

    p = (uint8_t *)b;
    memset(p, 0, bytes);
    *(unsigned *)p = c;
    return p + HEADER;
}

void kfree(void *ptr)
{
    uint8_t *p;
    struct block *b;

    if (!ptr)
        return;
    p = (uint8_t *)ptr - HEADER;
    b = (struct block *)p;
    unsigned c = *(unsigned *)p;
    unsigned long was = spin_lock_irqsave(&lock);
    b->next = free_list[c];
    free_list[c] = b;
    spin_unlock_irqrestore(&lock, was);
}

uint64_t kmalloc_pages(void)
{
    return pages_taken;
}
