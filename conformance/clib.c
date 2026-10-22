/*
 * conformance/clib.c -- lib/clib, C's library (V6.5)
 *
 * conformance/clibcheck.h's cases, built with include/clib's headers
 * and linked with libclib.a, as acpid is. Then errno is each task's own:
 * a second task finds it 0 while the first's is set, and sets its own
 * without changing the first's. A block it mallocs, the first frees:
 * one pool for the process. Then what the common calls cost. Exits 0,
 * or 1 with the failing line or step logged.
 */
#include <planar/exec.h>

#include "clibcheck.h"
#include "log.h"

static Handle first;
static void *volatile from_second;
static volatile int failed;

constexpr uint64_t DONE = 1ULL << 20;

static void second(uint64_t arg)
{
    (void)arg;
    if (errno != 0)
        failed = 1;                     /* the first task's, seen here */
    strtoul("12", nullptr, 1);          /* base 1: EINVAL */
    if (errno != EINVAL)
        failed = 2;
    if (!(from_second = malloc(100)))
        failed = 3;
    Signal(first, DONE);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    return (x > y) - (x < y);
}

/* What each costs, in nanoseconds. */
static void costs(void)
{
    constexpr unsigned N = 10000, SORT = 1000;
    static int v[SORT];
    volatile uint64_t sink = 0;
    uint64_t t0, mem, num, sort;
    uint64_t seed = 1;

    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++) {
        void *p = malloc(64);

        sink += (uintptr_t)p;
        free(p);
    }
    mem = (CurrentTime() - t0) / N;
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++)
        sink += strtoul("4294967295", nullptr, 10);
    num = (CurrentTime() - t0) / N;
    for (unsigned i = 0; i < SORT; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        v[i] = (int)(seed >> 33);
    }
    t0 = CurrentTime();
    qsort(v, SORT, sizeof v[0], cmp_int);
    sort = (CurrentTime() - t0) / 1000;
    KPrintF("clib: malloc and free of 64 bytes %lu ns, strtoul of 10 digits %lu ns, "
            "qsort of 1000 ints %lu us", mem, num, sort);
}

int main(void)
{
    unsigned cases = 0, line;

    if ((line = clib_check(&cases))) {
        KPrintF("clib: check at clibcheck.h:%u failed", line);
        return 1;
    }
    first = FindTask(nullptr);
    SetSignal(0, DONE);
    errno = ERANGE;
    if (CreateTask(second, 0) <= 0) {
        KPrintF("clib: no second task");
        return 1;
    }
    Wait(DONE);
    if (failed || errno != ERANGE) {
        KPrintF("clib: errno across tasks failed at step %d (the first's %d)", failed, errno);
        return 1;
    }
    free(from_second);
    KPrintF("clib: %u cases held: string, ctype, strtol, malloc, qsort, snprintf, stdbit; "
            "errno each task's own", cases);
    costs();
    return 0;
}
