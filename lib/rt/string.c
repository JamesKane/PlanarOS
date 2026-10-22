/*
 * lib/rt/string.c -- the memory functions compiled code may call
 *
 * Linked into every user program: the compiler turns struct copies and
 * zero initialisers into these calls even without a C library. They move
 * 8 bytes a step, by aligned words, with bytes for the head and the tail.
 * Where source and destination are differently aligned, memcpy shifts
 * aligned words together; memmove copying backwards takes bytes then.
 *
 * Each is weak: an ISA's own, by its SIMD unit, takes its place where
 * there is one (arch/<isa>/user/string.S). These are what the host test
 * checks (lib/rt/test.c), and what an ISA without one would run.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* A word that may alias anything. */
typedef uint64_t __attribute__((may_alias)) word;

static inline bool aligned(const void *p)
{
    return ((uintptr_t)p & 7) == 0;
}

/* Forwards, as memcpy and a memmove to a lower address need: each word
 * is read before the one at its address is written. A source out of
 * step with the destination is read by aligned words, so the last may
 * take up to 7 bytes past its end, within the word that holds its last
 * byte: never another page. */
[[gnu::no_sanitize("address")]]
static void copy_forwards(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n >= 16) {
        while (!aligned(d)) {
            *d++ = *s++;
            n--;
        }
        if (aligned(s)) {
            for (; n >= 32; n -= 32, d += 32, s += 32) {
                word a = ((const word *)s)[0], b = ((const word *)s)[1];
                word c = ((const word *)s)[2], e = ((const word *)s)[3];

                ((word *)d)[0] = a;
                ((word *)d)[1] = b;
                ((word *)d)[2] = c;
                ((word *)d)[3] = e;
            }
            for (; n >= 8; n -= 8, d += 8, s += 8)
                *(word *)d = *(const word *)s;
        } else {
            unsigned shift = (unsigned)((uintptr_t)s & 7) * 8;
            const word *w = (const word *)((uintptr_t)s & ~(uintptr_t)7);
            uint64_t lo = *w++;

            for (; n >= 8; n -= 8, d += 8, s += 8) {
                uint64_t hi = *w++;

                *(word *)d = lo >> shift | hi << (64 - shift);   /* little-endian */
                lo = hi;
            }
        }
    }
    while (n--)
        *d++ = *s++;
}

[[gnu::weak]] void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    copy_forwards(dst, src, n);
    return dst;
}

[[gnu::weak]] void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if ((uintptr_t)d - (uintptr_t)s >= n) {     /* below, or past the end */
        copy_forwards(d, s, n);
        return dst;
    }
    d += n;
    s += n;
    if (n >= 16 && ((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
        while (!aligned(d)) {
            *--d = *--s;
            n--;
        }
        for (; n >= 8; n -= 8) {
            d -= 8;
            s -= 8;
            *(word *)d = *(const word *)s;
        }
    }
    while (n--)
        *--d = *--s;
    return dst;
}

[[gnu::weak]] void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    if (n >= 16) {
        uint64_t v = (uint8_t)c * 0x0101010101010101ULL;

        while (!aligned(d)) {
            *d++ = (uint8_t)c;
            n--;
        }
        for (; n >= 32; n -= 32, d += 32) {
            ((word *)d)[0] = v;
            ((word *)d)[1] = v;
            ((word *)d)[2] = v;
            ((word *)d)[3] = v;
        }
        for (; n >= 8; n -= 8, d += 8)
            *(word *)d = v;
    }
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

/* By words while both are in step, to the first that differs; then by
 * bytes, which say which way. */
[[gnu::weak]] int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;

    if (n >= 16 && ((uintptr_t)p & 7) == ((uintptr_t)q & 7)) {
        while (!aligned(p)) {
            if (*p != *q)
                return *p - *q;
            p++, q++, n--;
        }
        for (; n >= 8 && *(const word *)p == *(const word *)q; n -= 8)
            p += 8, q += 8;
    }
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}
