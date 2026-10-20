/*
 * lib/rt/string.c -- the memory functions compiled code may call
 *
 * Linked into every user program: the compiler turns struct copies and
 * zero initialisers into these calls even without a C library.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

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
