/*
 * conformance/dosargs.c -- dos.library's ReadArgs and path calls (V6.6)
 *
 * conformance/argscheck.h's cases, in a process, over exec.library's
 * AllocVec, as lib/dos/test.c runs them on the host. No dos server is
 * needed: these are the strings only. Then what ReadArgs costs on a
 * command's line. Exits 0, or 1 with the failing line logged.
 */
#include <planar/dos.h>
#include <planar/exec.h>

#include "argscheck.h"
#include "log.h"

int main(void)
{
    constexpr unsigned N = 2000;
    unsigned cases = 0, line;
    struct RDArgs ra = { 0 };
    int64_t a[4];
    uint64_t t0, each;

    if ((line = args_check(&cases))) {
        KPrintF("dosargs: check at argscheck.h:%u failed", line);
        return 1;
    }
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++) {
        ra.rda_Source = "SYS:EFI/planar/S/greeting \"a b\" TO=SYS:x NUMBER";
        if (ReadArgs("FROM/A/M,TO/K,NUMBER/S", a, &ra) != 0)
            return 1;
        FreeArgs(&ra);
    }
    each = (CurrentTime() - t0) / N;
    KPrintF("dosargs: %u cases held: ReadArgs's modifiers, quotes and reasons, AddPart, "
            "FilePart, PathPart; ReadArgs of a 4-word line %lu ns", cases, each);
    return 0;
}
