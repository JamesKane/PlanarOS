/*
 * conformance/copyrace.c -- the kernel's copies to user memory under ProtectArea (§10.8)
 *
 * One task keeps giving an Area the rights it has, which remaps every
 * page, break before make; the other keeps asking QueryArea to write its
 * answer into that Area. A kernel copy that lands in the window took the
 * fixup at once and failed with ERR_BADADDR, though the page was there a
 * moment later: it must find the mapping again, as a user access does.
 * Exits 0, or 1 if a copy failed.
 */
#include <stdatomic.h>

#include <planar/exec.h>

constexpr unsigned ROUNDS = 20000;

static atomic_bool done;
static int64_t area;

static void remapper(uint64_t)
{
    while (!atomic_load(&done))
        ProtectArea((Handle)area, AREAR_READ | AREAR_WRITE);
    ExitTask(0);
}

int main(void)
{
    struct AreaInfo *info;
    unsigned failed = 0;

    if ((area = AllocArea(4096, 0, (void **)&info)) < 0 || CreateTask(remapper, 0) < 0)
        return 2;
    for (unsigned i = 0; i < ROUNDS; i++)
        if (QueryArea(info, info) != 0)
            failed++;
    atomic_store(&done, true);
    return failed ? 1 : 0;
}
