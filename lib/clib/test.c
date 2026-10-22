/*
 * lib/clib/test.c -- lib/clib on the host
 *
 * lib/clib's files, renamed clib_* so as not to be the host's (the
 * Makefile's clib-test), with the sanitizers, against
 * conformance/clibcheck.h's cases, which conformance/clib.c runs in the
 * guest. malloc's AllocVec is lib/exec/pool.c's, over Areas the host's
 * aligned_alloc stands in for. Then testhost.c checks the strtol
 * family, the string functions and qsort against the host's own on
 * random inputs.
 * This file sees include/clib's headers, so the host's few are declared
 * by hand.
 */
#include <planar/exec.h>

#include "clibcheck.h"

/* The host's, now clibcheck.h has the renamed ones. */
#undef free
#undef abort
int printf(const char *fmt, ...);
void *aligned_alloc(size_t alignment, size_t size);
void free(void *p);
[[noreturn]] void abort(void);

unsigned clib_vs_host(unsigned *cases);

constexpr unsigned AREAS = 4096;

static void *area_at[AREAS];

int64_t AllocArea(uint64_t size, uint32_t flags, void **address)
{
    (void)flags;
    for (unsigned h = 1; h < AREAS; h++)
        if (!area_at[h]) {
            if (!(area_at[h] = aligned_alloc(4096, size)))
                return -1;
            memset(area_at[h], 0, size);
            *address = area_at[h];
            return h;
        }
    return -1;
}

int64_t FreeArea(Handle area)
{
    if (area >= AREAS || !area_at[area]) {
        printf("clib-test: FAILED: FreeArea of %u, not an Area\n", area);
        abort();
    }
    free(area_at[area]);
    area_at[area] = nullptr;
    return 0;
}

/* One task: the lock is never contended, and nothing exits. */
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

int64_t KLog(const char *text)
{
    return printf("clib-test: %s\n", text);
}

void ExitProcess(int64_t code)
{
    printf("clib-test: FAILED: exited %lld\n", (long long)code);
    abort();
}

int main(void)
{
    unsigned cases = 0, against = 0, line;

    if ((line = clib_check(&cases))) {
        printf("clib-test: FAILED: clibcheck.h:%u\n", line);
        return 1;
    }
    if ((line = clib_vs_host(&against))) {
        printf("clib-test: FAILED: testhost.c:%u\n", line);
        return 1;
    }
    printf("clib-test: ok: %u cases, %u against the host's\n", cases, against);
    return 0;
}
