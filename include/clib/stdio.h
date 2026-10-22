/*
 * clib/stdio.h -- C's formatting into a buffer, and nothing else (docs §17.4.5)
 *
 * No FILE, no printf: a program writes through dos.library. snprintf
 * and vsnprintf are utility.library's VSNPrintf, so their conversions
 * are RawDoFmt's: C's, but floating point.
 */
#ifndef CLIB_STDIO_H
#define CLIB_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

int snprintf(char *restrict s, size_t n, const char *restrict format, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *restrict s, size_t n, const char *restrict format, va_list args)
    __attribute__((format(printf, 3, 0)));

#endif /* CLIB_STDIO_H */
