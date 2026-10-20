/*
 * conformance/arearace.c -- FreeArea racing calls on the same handle
 *
 * Two tasks of one process: one allocates an Area and keeps sharing it
 * into its own process and changing its rights; the other frees it.
 * However they interleave, a call must never map an Area once it has
 * been freed (docs/10-code-review.md C1): sharing it back into this
 * process may fail with ERR_EXISTS while the owner still maps it, but
 * never succeed. The kernel's bad-free and leak checks catch the rest.
 * Exits 0, or 1 if a share ever succeeded.
 */
#include <stdatomic.h>

#include <planar/exec.h>
#include <planar/syscalls.h>

constexpr unsigned ROUNDS = 3000;
constexpr uint64_t PAGE = 4096;

static atomic_uint phase;           /* odd: allocated; even: freed */
static atomic_uint current;

static void freer(uint64_t)
{
    for (unsigned r = 0; r < ROUNDS; r++) {
        while (atomic_load(&phase) != 2 * r + 1)
            ;
        FreeArea((Handle)atomic_load(&current));
        atomic_store(&phase, 2 * r + 2);
    }
    ExitTask(0);
}

int main(void)
{
    Handle me = FindTask(nullptr);
    unsigned bad = 0;

    if (!CreateTask(freer, 0))
        return 2;
    for (unsigned r = 0; r < ROUNDS; r++) {
        void *p;
        int64_t h = AllocArea(2 * PAGE, 0, &p);

        if (h <= 0)
            return 3;
        atomic_store(&current, (unsigned)h);
        atomic_store(&phase, 2 * r + 1);
        while (atomic_load(&phase) != 2 * r + 2) {
            bad += ShareArea((Handle)h, me, AREAR_READ | AREAR_WRITE) == 0;
            ProtectArea((Handle)h, AREAR_READ);
            ProtectArea((Handle)h, AREAR_READ | AREAR_WRITE);
        }
        bad += ShareArea((Handle)h, me, AREAR_READ) == 0;
    }
    return bad ? 1 : 0;
}
