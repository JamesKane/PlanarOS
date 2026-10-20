/*
 * kernel/kstring.c -- the memory and string functions the kernel uses
 *
 * Even freestanding, the compiler turns struct copies and zero
 * initialisers into calls to the first four. memset and memcpy go a word
 * at a time where they can: every page handed out is zeroed with memset.
 */
#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

typedef uint64_t word_t __attribute__((may_alias));

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint64_t w = (uint8_t)c * 0x0101010101010101ULL;

    while (n && ((uintptr_t)d & 7)) {
        *d++ = (uint8_t)c;
        n--;
    }
    for (; n >= 32; n -= 32, d += 32) {
        ((word_t *)d)[0] = w;
        ((word_t *)d)[1] = w;
        ((word_t *)d)[2] = w;
        ((word_t *)d)[3] = w;
    }
    for (; n >= 8; n -= 8, d += 8)
        *(word_t *)d = w;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        for (; n >= 8; n -= 8, d += 8, s += 8)
            *(word_t *)d = *(const word_t *)s;
    }
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
