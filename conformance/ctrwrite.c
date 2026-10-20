/*
 * conformance/ctrwrite.c -- user mode reads the cycle counter, never controls it
 *
 * Reads the counter, which must work, then tries to change its controls,
 * which must fault as an illegal instruction: otherwise any process
 * could stop the counters every other one reads (docs/10-code-review.md
 * M2). Exits 1 if the write went through.
 */
#include <planar/exec.h>

#include "cycles.h"

int main(void)
{
    uint64_t c0 = ReadCycles();

    if (ReadCycles() == c0 && ReadCycles() == c0)
        return 2;                   /* not counting at all */
    CounterWrite();
    return 1;
}
