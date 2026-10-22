/*
 * lib/utility/test.c -- utility.library on the host
 *
 * lib/utility's files and lib/exec/fmt.c with the sanitizers, against
 * conformance/utilcheck.h's cases, which conformance/utility.c runs in
 * the guest. The Span readers have their fuzzer too (tools/span-fuzz).
 */
#include <stdio.h>

#include "utilcheck.h"

int main(void)
{
    unsigned cases = 0, line;

    if ((line = util_check(&cases))) {
        printf("utility-test: FAILED: utilcheck.h:%u\n", line);
        return 1;
    }
    printf("utility-test: ok: %u cases\n", cases);
    return 0;
}
