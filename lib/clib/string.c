/*
 * lib/clib/string.c -- C's string functions (docs §17.4.5)
 *
 * A byte at a time: the third-party code that wants these isn't where
 * Planar's time goes. The memory functions are lib/rt's. Comparisons
 * are of the bytes as unsigned, as C says.
 */
#include <stdint.h>

#include <string.h>

void *memset_explicit(void *dst, int c, size_t n)
{
    memset(dst, c, n);
    __asm__ volatile("" : : "r"(dst) : "memory");     /* the stores are seen */
    return dst;
}

void *memchr(const void *s, int c, size_t n)
{
    for (const uint8_t *p = s; n; n--, p++)
        if (*p == (uint8_t)c)
            return (void *)p;
    return nullptr;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t n)
{
    size_t i = 0;

    while (i < n && s[i])
        i++;
    return i;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    return 0;
}

char *strcpy(char *restrict dst, const char *restrict src)
{
    char *d = dst;

    while ((*d++ = *src++))
        ;
    return dst;
}

/* C's: n bytes are written, NULs after the string; no NUL if it didn't fit. */
char *strncpy(char *restrict dst, const char *restrict src, size_t n)
{
    size_t i = 0;

    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *restrict dst, const char *restrict src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

/* At most n of src's bytes, then a NUL, always. */
char *strncat(char *restrict dst, const char *restrict src, size_t n)
{
    char *d = dst + strlen(dst);

    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

/* strchr(s, 0) finds the NUL, as C says. */
char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return nullptr;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = nullptr;

    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);

    if (!n)
        return (char *)haystack;
    for (; *haystack; haystack++)
        if (*haystack == *needle && !strncmp(haystack, needle, n))
            return (char *)haystack;
    return nullptr;
}
