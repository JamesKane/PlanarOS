/*
 * conformance/protrace.c -- ProtectArea while another task writes
 *
 * One task keeps writing to an Area, so its CPU caches the mapping; the
 * other makes the Area read-only and then lets the writer write once
 * more. That write must fault wherever the writer runs: changing a
 * mapping takes it away on every CPU (docs/10-code-review.md H2). Exits
 * 1 if the write after the change went through.
 */
#include <stdatomic.h>

#include <planar/exec.h>

static atomic_uint_fast64_t writes;
static atomic_bool go;
static volatile uint64_t *word;

static void writer(uint64_t)
{
    while (!atomic_load(&go)) {
        *word = atomic_load(&writes);
        atomic_fetch_add(&writes, 1);
    }
    *word = 0;                      /* read-only by now: this faults */
    atomic_store(&writes, ~0ULL);
    for (;;)
        ;
}

int main(void)
{
    void *p;
    int64_t h = AllocArea(4096, 0, &p);

    if (h <= 0)
        return 2;
    word = p;
    if (!CreateTask(writer, 0))
        return 3;
    while (atomic_load(&writes) < 100000)
        ;
    if (ProtectArea((Handle)h, AREAR_READ) != 0)
        return 4;
    atomic_store(&go, true);
    while (atomic_load(&writes) != ~0ULL)
        ;
    return 1;
}
