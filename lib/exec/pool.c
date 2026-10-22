/*
 * lib/exec/pool.c -- memory pools and AllocVec (docs §17.4.2)
 *
 * Exec's V39 pools. A pool hands out blocks from puddles, Areas of the
 * puddle size, a bump pointer at a time; a block freed goes on its size
 * class's list for the next of that class. Classes are 16-byte steps to
 * 256 bytes, then four to each doubling, so a block wastes at most a
 * fifth of itself, and FreePooled's size names the class: no header. An
 * allocation above the threshold is an Area of its own, on the pool's
 * list, so DeletePool frees it with the puddles. The pool's own header
 * and lists are at the start of its first puddle, or of the buffer a
 * CreatePoolMem pool is made in.
 *
 * AllocVec's blocks have a 16-byte header saying how big, and come from
 * a pool the process makes at its first call, shared by its tasks. The
 * pool is named in the process's own memory (procdata.h), so this file's
 * AllocVec, linked into a program, and exec.library's, shared, use the
 * same one.
 */
#include <planar/exec.h>
#include <stdatomic.h>

#include "procdata.h"

constexpr uint64_t PAGE = 4096;
constexpr unsigned SMALL_CLASSES = 16;          /* 16 to 256 bytes */
constexpr uint32_t AREA_FLAGS = AREAF_DMA32 | AREAF_NC;

struct puddle {
    struct puddle *next;
    Handle area;
    uint32_t pad;
};

/* An allocation above the threshold: its Area, recorded in a block. */
struct big {
    struct big *next;
    void *at;
    Handle area;
};

struct Pool {
    _Atomic uint64_t lock;          /* 0 free, 1 held, 2 held with waiters */
    uint32_t flags;
    uint32_t classes;
    uint64_t puddle;                /* 0: a fixed buffer, which doesn't grow */
    uint64_t threshold;
    uint8_t *next;                  /* the bump pointer, and what's left after it */
    uint64_t left;
    struct puddle *puddles;
    struct big *bigs;
    void *free[];                   /* a list per class */
};

static uint64_t round_up(uint64_t n, uint64_t to)
{
    return (n + to - 1) & ~(to - 1);
}

/* The class of 'n' bytes (n > 0), and its blocks' size. */
static unsigned class_of(uint64_t n)
{
    unsigned k;

    if (n <= 256)
        return (unsigned)((n + 15) / 16 - 1);
    k = 63 - (unsigned)__builtin_clzll(n - 1);  /* n is in (2^k, 2^(k+1)] */
    return SMALL_CLASSES + (k - 8) * 4 + (unsigned)(((n - 1) >> (k - 2)) - 4);
}

static uint64_t class_size(unsigned c)
{
    if (c < SMALL_CLASSES)
        return (c + 1) * 16ULL;
    c -= SMALL_CLASSES;
    return (5ULL + c % 4) << (c / 4 + 6);
}

/* A lock between the tasks of one process (WaitAddr's rule, §12.5). */
static void lock(struct Pool *p)
{
    uint64_t c = 0;

    if (!(p->flags & POOLF_LOCKED) ||
        atomic_compare_exchange_strong_explicit(&p->lock, &c, 1, memory_order_acquire,
                                                memory_order_relaxed))
        return;
    if (c != 2)
        c = atomic_exchange_explicit(&p->lock, 2, memory_order_acquire);
    while (c != 0) {
        WaitAddr((const volatile uint64_t *)&p->lock, 2, 0);
        c = atomic_exchange_explicit(&p->lock, 2, memory_order_acquire);
    }
}

static void unlock(struct Pool *p)
{
    if ((p->flags & POOLF_LOCKED) &&
        atomic_exchange_explicit(&p->lock, 0, memory_order_release) == 2)
        WakeAddr((const volatile uint64_t *)&p->lock, 1);
}

/* The header and its lists, at 'at', with 'size' bytes there to bump from. */
static struct Pool *init(void *at, uint64_t size, uint32_t flags, uint64_t puddle,
                         uint64_t threshold)
{
    unsigned classes = class_of(threshold > sizeof(struct big) ? threshold : sizeof(struct big)) + 1;
    uint64_t head = round_up(sizeof(struct Pool) + classes * sizeof(void *), 16);
    struct Pool *p = at;

    if (head > size)
        return nullptr;
    SetMem(p, 0, head);
    p->flags = flags;
    p->classes = classes;
    p->puddle = puddle;
    p->threshold = threshold;
    p->next = (uint8_t *)at + head;
    p->left = size - head;
    return p;
}

struct Pool *CreatePool(uint32_t flags, uint64_t puddleSize, uint64_t threshSize)
{
    uint64_t puddle = round_up(puddleSize, PAGE);
    struct puddle *first;
    struct Pool *p;
    void *at;
    int64_t a;

    if ((flags & ~(AREAF_CLEAR | AREAF_CONTIG | AREA_FLAGS | POOLF_LOCKED)) || !threshSize ||
        threshSize > puddleSize || class_size(class_of(threshSize)) > puddle - sizeof(struct puddle))
        return nullptr;
    if ((a = AllocArea(puddle, flags & AREA_FLAGS, &at)) < 0)
        return nullptr;
    first = at;
    p = init(first + 1, puddle - sizeof *first, flags, puddle, threshSize);
    if (!p) {
        FreeArea((Handle)a);
        return nullptr;
    }
    first->area = (Handle)a;
    p->puddles = first;
    return p;
}

struct Pool *CreatePoolMem(void *buf, uint64_t size)
{
    uint8_t *at = (uint8_t *)round_up((uintptr_t)buf, 16);
    uint64_t skip = (uint64_t)(at - (uint8_t *)buf);

    if (size < skip + sizeof(struct Pool) + 16)
        return nullptr;
    size -= skip;
    return init(at, size, 0, 0, size);
}

void DeletePool(struct Pool *p)
{
    struct puddle *u, *next;

    if (!p)
        return;
    for (struct big *b = p->bigs; b; b = b->next)
        FreeArea(b->area);
    /* The first puddle, holding the header, is last on the list. */
    for (u = p->puddles; u; u = next) {
        next = u->next;
        FreeArea(u->area);
    }
}

/* A block of class 'c': from its list, or bumped from the puddle. */
static void *alloc_small(struct Pool *p, unsigned c)
{
    uint64_t n = class_size(c);
    void *m;

    if ((m = p->free[c])) {
        p->free[c] = *(void **)m;
        if (p->flags & AREAF_CLEAR)
            SetMem(m, 0, n);
        return m;
    }
    if (p->left < n) {
        struct puddle *u;
        void *at;
        int64_t a;

        /* What's left of this one stays unused: under a class's size. */
        if (!p->puddle || (a = AllocArea(p->puddle, p->flags & AREA_FLAGS, &at)) < 0)
            return nullptr;
        u = at;
        u->area = (Handle)a;
        u->next = p->puddles;
        p->puddles = u;
        p->next = (uint8_t *)(u + 1);
        p->left = p->puddle - sizeof *u;
    }
    m = p->next;
    p->next += n;
    p->left -= n;
    if (!p->puddle && (p->flags & AREAF_CLEAR))
        SetMem(m, 0, n);                        /* a puddle's are fresh, and zero */
    return m;
}

static void *alloc_big(struct Pool *p, uint64_t n)
{
    struct big *b = alloc_small(p, class_of(sizeof *b));
    int64_t a;

    if (!b)
        return nullptr;
    if ((a = AllocArea(round_up(n, PAGE), p->flags & AREA_FLAGS, &b->at)) < 0) {
        *(void **)b = p->free[class_of(sizeof *b)];
        p->free[class_of(sizeof *b)] = b;
        return nullptr;
    }
    b->area = (Handle)a;
    b->next = p->bigs;
    p->bigs = b;
    return b->at;
}

void *AllocPooled(struct Pool *p, uint64_t memSize)
{
    void *m;

    if (!p || !memSize)
        return nullptr;
    lock(p);
    if (memSize > p->threshold)
        m = p->puddle ? alloc_big(p, memSize) : nullptr;
    else
        m = alloc_small(p, class_of(memSize));
    unlock(p);
    return m;
}

void FreePooled(struct Pool *p, void *memory, uint64_t memSize)
{
    unsigned c;

    if (!p || !memory || !memSize)
        return;
    lock(p);
    if (memSize > p->threshold) {
        struct big **l = &p->bigs, *b;

        while ((b = *l) && b->at != memory)
            l = &b->next;
        if (!b) {                               /* not one of this pool's */
            unlock(p);
            return;
        }
        *l = b->next;
        FreeArea(b->area);
        memory = b;                             /* and its record goes back */
        memSize = sizeof *b;
    }
    c = class_of(memSize);
    *(void **)memory = p->free[c];
    p->free[c] = memory;
    unlock(p);
}

/*
 * AllocVec: a header, then the block. 'area' is set when the block is an
 * Area of its own (AREAF_DMA32 or AREAF_NC asked for); otherwise it's
 * from the process's pool, 'size' bytes with the header.
 */
struct vec {
    uint64_t size;
    Handle area;
    uint32_t pad;
};
static_assert(sizeof(struct vec) == 16);

constexpr uint64_t VEC_PUDDLE = 64 * 1024, VEC_THRESHOLD = 16 * 1024;

void *AllocVec(uint64_t byteSize, uint32_t requirements)
{
    uint64_t n = byteSize + sizeof(struct vec);
    struct Pool *_Atomic *slot = &process_data()->pd_VecPool;
    struct Pool *p, *none = nullptr;
    struct vec *v;

    if (!byteSize || n < byteSize ||
        (requirements & ~(AREAF_CLEAR | AREAF_CONTIG | AREA_FLAGS)))
        return nullptr;
    if (requirements & AREA_FLAGS) {
        void *at;
        int64_t a = AllocArea(round_up(n, PAGE), requirements & AREA_FLAGS, &at);

        if (a < 0)
            return nullptr;
        v = at;
        *v = (struct vec){ n, (Handle)a, 0 };
        return v + 1;                           /* an Area is zero already */
    }
    if (!(p = atomic_load_explicit(slot, memory_order_acquire))) {
        /* The first call; two tasks making it at once keep one pool. */
        if (!(p = CreatePool(POOLF_LOCKED, VEC_PUDDLE, VEC_THRESHOLD)))
            return nullptr;
        if (!atomic_compare_exchange_strong_explicit(slot, &none, p, memory_order_acq_rel,
                                                     memory_order_acquire)) {
            DeletePool(p);
            p = none;
        }
    }
    if (!(v = AllocPooled(p, n)))
        return nullptr;
    v->size = n;
    v->area = 0;
    if (requirements & AREAF_CLEAR)
        SetMem(v + 1, 0, byteSize);
    return v + 1;
}

void FreeVec(void *memoryBlock)
{
    struct vec *v;

    if (!memoryBlock)
        return;
    v = (struct vec *)memoryBlock - 1;
    if (v->area)
        FreeArea(v->area);
    else
        FreePooled(atomic_load_explicit(&process_data()->pd_VecPool, memory_order_acquire), v,
                   v->size);
}
