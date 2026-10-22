/*
 * lib/exec/test.c -- pools, AllocVec and RawDoFmt on the host
 *
 * lib/exec/pool.c and fmt.c with the sanitizers, over Areas the host's
 * aligned_alloc stands in for, counted so a pool's leaks show. The
 * formatter's cases are conformance/fmtcheck.h's, which pool.c runs in
 * the guest. Then pools churned at random: every block's bytes checked
 * before it's freed, and every Area gone after DeletePool.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fmtcheck.h"

constexpr unsigned AREAS = 4096;

static void *area_at[AREAS];
static unsigned areas_live;

int64_t AllocArea(uint64_t size, uint32_t flags, void **address)
{
    (void)flags;
    for (unsigned h = 1; h < AREAS; h++)
        if (!area_at[h]) {
            if (!(area_at[h] = aligned_alloc(4096, size)))
                return -1;
            memset(area_at[h], 0, size);
            *address = area_at[h];
            areas_live++;
            return h;
        }
    return -1;
}

int64_t FreeArea(Handle area)
{
    if (area >= AREAS || !area_at[area]) {
        printf("exec-test: FAILED: FreeArea of %u, not an Area\n", area);
        exit(1);
    }
    free(area_at[area]);
    area_at[area] = nullptr;
    areas_live--;
    return 0;
}

/* One task: the lock is never contended. */
int64_t WaitAddr(const volatile uint64_t *address, uint64_t value, uint64_t deadline)
{
    (void)address, (void)value, (void)deadline;
    abort();
}

int64_t WakeAddr(const volatile uint64_t *address, uint32_t count)
{
    (void)address, (void)count;
    abort();
}

struct live {
    uint8_t *p;
    uint64_t n;
    uint8_t tag;
};

static uint64_t seed = 0x9e3779b97f4a7c15;

static uint64_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

/* A pool churned: 0, or what went wrong. */
static const char *churn(uint32_t flags, uint64_t puddle, uint64_t threshold, unsigned *ops)
{
    static struct live live[512];
    struct Pool *p = CreatePool(flags, puddle, threshold);

    if (!p)
        return "CreatePool";
    memset(live, 0, sizeof live);
    for (unsigned r = 0; r < 200000; r++, (*ops)++) {
        struct live *l = &live[rnd() % 512];

        if (l->p) {
            for (uint64_t i = 0; i < l->n; i++)
                if (l->p[i] != (uint8_t)(l->tag + i))
                    return "a block changed under its owner";
            FreePooled(p, l->p, l->n);
            l->p = nullptr;
            continue;
        }
        /* Mostly small, now and then above the threshold. */
        l->n = rnd() % 16 ? 1 + rnd() % threshold : threshold + 1 + rnd() % (3 * puddle);
        if (!(l->p = AllocPooled(p, l->n)) || ((uintptr_t)l->p & 15))
            return "AllocPooled";
        if (flags & AREAF_CLEAR)
            for (uint64_t i = 0; i < l->n; i++)
                if (l->p[i])
                    return "AREAF_CLEAR's block not zero";
        l->tag = (uint8_t)rnd();
        for (uint64_t i = 0; i < l->n; i++)
            l->p[i] = (uint8_t)(l->tag + i);
    }
    DeletePool(p);
    return areas_live ? "an Area outlived DeletePool" : nullptr;
}

int main(void)
{
    struct fmtcheck_fail ff;
    unsigned fmts = 0, ops = 0;
    const char *why;
    static uint8_t buf[3000];
    struct Pool *p;
    unsigned n = 0;
    void *v;

    if (fmt_check(&ff, &fmts)) {
        printf("exec-test: FAILED: fmtcheck.h:%u wrote \"%s\", not \"%s\"\n", ff.line, ff.got,
               ff.want);
        return 1;
    }
    if ((why = churn(0, 16384, 1024, &ops)) || (why = churn(AREAF_CLEAR, 4096, 3000, &ops)) ||
        (why = churn(POOLF_LOCKED, 65536, 16, &ops))) {
        printf("exec-test: FAILED: %s\n", why);
        return 1;
    }
    /* A fixed buffer: blocks of every size until it's full, all inside. */
    if (!(p = CreatePoolMem(buf + 1, sizeof buf - 1)))
        return printf("exec-test: FAILED: CreatePoolMem\n"), 1;
    while ((v = AllocPooled(p, 1 + n % 200))) {
        if ((uint8_t *)v < buf || (uint8_t *)v + 1 + n % 200 > buf + sizeof buf)
            return printf("exec-test: FAILED: a fixed pool's block outside it\n"), 1;
        memset(v, 0xee, 1 + n % 200);
        n++;
    }
    DeletePool(p);
    /* AllocVec: blocks of every size; twice, the second from its lists. */
    for (unsigned pass = 0; pass < 2; pass++) {
        unsigned before = areas_live;

        for (uint64_t s = 1; s < 40000; s += 1 + s / 8) {
            uint8_t *a = AllocVec(s, AREAF_CLEAR);

            if (!a || ((uintptr_t)a & 15))
                return printf("exec-test: FAILED: AllocVec(%llu)\n", (unsigned long long)s), 1;
            for (uint64_t i = 0; i < s; i++)
                if (a[i])
                    return printf("exec-test: FAILED: AllocVec's AREAF_CLEAR\n"), 1;
            memset(a, 0x5a, s);
            FreeVec(a);
        }
        if (pass && areas_live != before)
            return printf("exec-test: FAILED: AllocVec took %u Areas again\n",
                          areas_live - before), 1;
    }
    printf("exec-test: ok: RawDoFmt %u cases; pools churned %u times; %u blocks in a fixed buffer\n",
           fmts, ops, n);
    return 0;
}
