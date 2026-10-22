/*
 * lib/clib/assert.c -- where a failed assert ends (docs §17.4.5)
 */
#include <planar/exec.h>
#include <planar/utility.h>

#include <assert.h>
#include <stdlib.h>

void __clib_assert(const char *expr, const char *file, int line, const char *func)
{
    char text[200];

    SNPrintf(text, sizeof text, "assertion failed: %s, at %s:%d, in %s", expr, file, line, func);
    KLog(text);
    ExitProcess(EXIT_FAILURE);
}
