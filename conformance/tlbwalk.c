/*
 * conformance/tlbwalk.c -- touching more pages than the TLB holds (VQ3)
 *
 * Areas of 64 pages, as many as the argument's bits 16 and up say (at
 * most 192: 48 MiB), and one word of every page written, in the same
 * random order each round: one round to warm up, then as many as its low
 * 16 bits say, timed. The kernel counts the TLB refills
 * around it (V3.6 tlb); a run of 1 round taken from a run of 9 leaves
 * 8 rounds without the first touches' faults. Exits with the timed
 * rounds' cycles per touch, in tenths; -1 for a bad argument, or minus
 * the number of the Area it couldn't allocate.
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "cycles.h"

constexpr uint64_t PAGE = 4096;
constexpr unsigned MAX_AREAS = 192, PAGES = 64;

static uint16_t order[MAX_AREAS * PAGES];

int main(uint64_t arg)
{
    volatile uint64_t *area[MAX_AREAS];
    Handle h[MAX_AREAS];
    uint64_t rng = 0x2545f4914f6cdd1dULL, rounds = arg & 0xffff, c0 = 0;
    unsigned areas = (unsigned)(arg >> 16), pages = areas * PAGES;

    if (areas == 0 || areas > MAX_AREAS)
        return -1;
    for (unsigned i = 0; i < areas; i++) {
        void *p;
        int64_t r = AllocArea(PAGES * PAGE, 0, &p);

        if (r < 0)
            return -(int)i - 1;
        h[i] = (Handle)r;
        area[i] = p;
    }
    for (unsigned i = 0; i < pages; i++)
        order[i] = (uint16_t)i;
    for (unsigned i = pages - 1; i > 0; i--) {     /* Fisher-Yates */
        unsigned j;
        uint16_t t;

        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        j = (unsigned)(rng % (i + 1));
        t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (uint64_t r = 0; r <= rounds; r++) {
        if (r == 1)
            c0 = ReadCycles();
        for (unsigned i = 0; i < pages; i++)
            area[order[i] / PAGES][order[i] % PAGES * (PAGE / 8)] += 1;
    }
    c0 = ReadCycles() - c0;
    for (unsigned i = 0; i < areas; i++)
        FreeArea(h[i]);
    return rounds ? (int)(c0 * 10 / (rounds * pages)) : 0;
}
