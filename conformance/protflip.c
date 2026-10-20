/*
 * conformance/protflip.c -- ProtectArea while a sibling task reads
 *
 * Changing a mapping takes it away for a moment (break before make). A
 * task reading the page just then must not die for it: the kernel sees
 * the page mapped again and retries (docs/10-code-review.md M6). One
 * task reads an Area in a loop while the other flips it between read
 * and read-write. Exits 0, or with a number if a call failed.
 */
#include <stdatomic.h>

#include <planar/exec.h>

constexpr unsigned FLIPS = 2000;

static atomic_bool done, reading;
static volatile uint64_t *word;
static uint64_t sum;

static void reader(uint64_t)
{
    atomic_store(&reading, true);
    while (!atomic_load(&done))
        sum += *word;
    ExitTask(0);
}

int main(void)
{
    void *p;
    int64_t h = AllocArea(4096, 0, &p);

    if (h <= 0)
        return 2;
    word = p;
    if (CreateTask(reader, 0) <= 0)
        return 3;
    while (!atomic_load(&reading))
        ;
    for (unsigned i = 0; i < FLIPS; i++)
        if (ProtectArea((Handle)h, i & 1 ? AREAR_READ | AREAR_WRITE : AREAR_READ) != 0)
            return 4;
    atomic_store(&done, true);
    return 0;
}
