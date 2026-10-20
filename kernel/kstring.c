/*
 * kernel/kstring.c -- the memory and string functions the kernel uses
 *
 * Even freestanding, the compiler turns struct copies and zero
 * initialisers into calls to the first four. Byte at a time for now; the
 * kernel uses them little.
 */
#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;

    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t kstrlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

bool kstreq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
