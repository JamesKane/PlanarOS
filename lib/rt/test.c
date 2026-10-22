/*
 * lib/rt/test.c -- lib/rt/string.c's word loops on the host
 *
 * The board's programs run their ISA's own (arch/<isa>/user/string.S);
 * these are what's left where an ISA has none. string.c is built with
 * its functions renamed rt_* (the Makefile's rt-test), and checked by
 * the same cases conformance/memfns.c runs in the guest.
 */
#include <stdio.h>

#define MEMCHECK_PREFIX rt_
#include "memcheck.h"

int main(void)
{
    struct memcheck_fail f;
    uint64_t cases = 0;

    if (memcheck(&f, &cases)) {
        printf("rt-test: FAILED: %s wrong at length %u, source +%u, destination +%u\n", f.fn,
               f.len, f.salign, f.dalign);
        return 1;
    }
    printf("rt-test: ok: lib/rt/string.c's word loops, %llu cases\n", (unsigned long long)cases);
    return 0;
}
