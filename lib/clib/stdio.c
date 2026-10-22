/*
 * lib/clib/stdio.c -- snprintf and vsnprintf (docs §17.4.5)
 *
 * utility.library's VSNPrintf, which already returns what C's does,
 * but in 64 bits: a length past INT_MAX is -1, as C says.
 */
#include <limits.h>

#include <planar/utility.h>

#include <stdio.h>

int vsnprintf(char *restrict s, size_t n, const char *restrict format, va_list args)
{
    int64_t r = VSNPrintf(s, n, format, args);

    return r > INT_MAX ? -1 : (int)r;
}

int snprintf(char *restrict s, size_t n, const char *restrict format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsnprintf(s, n, format, ap);
    va_end(ap);
    return r;
}
