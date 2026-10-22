/*
 * conformance/clibcheck.h -- lib/clib against what C says
 *
 * The string functions at their edges (signs above 0x7f, strncpy's
 * padding, strncat's NUL, strchr finding the NUL), ctype for every byte
 * and EOF, the strtol family's prefixes, ranges and end pointers,
 * malloc's alignment, realloc's room and copy, qsort and bsearch on many
 * sizes, and stdbit.h against loops. Shared by conformance/clib.c, in
 * the guest, and lib/clib/test.c, on the host with the sanitizers.
 * Built with include/clib's headers.
 */
#ifndef CONFORMANCE_CLIBCHECK_H
#define CONFORMANCE_CLIBCHECK_H

#include <stdint.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/exec/vec.h"

#define CC(cond) \
    if (!(cond)) \
        return __LINE__; \
    (*cases)++

static unsigned cc_strings(unsigned *cases)
{
    char buf[16];

    CC(strlen("") == 0 && strlen("abc") == 3);
    CC(strnlen("abc", 2) == 2 && strnlen("abc", 9) == 3 && strnlen("", 0) == 0);
    CC(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 && strcmp("abd", "abc") > 0);
    CC(strcmp("ab", "abc") < 0 && strcmp("abc", "ab") > 0);
    CC(strcmp("\x80", "\x7f") > 0);                 /* compared as unsigned */
    CC(strncmp("abcx", "abcy", 3) == 0 && strncmp("abcx", "abcy", 4) < 0);
    CC(strncmp("a", "b", 0) == 0 && strncmp("ab", "ab\0x", 9) == 0);
    CC(strcpy(buf, "hello") == buf && !strcmp(buf, "hello"));
    CC(strcat(buf, ", you") == buf && !strcmp(buf, "hello, you"));
    memset(buf, 'z', sizeof buf);
    CC(strncpy(buf, "ab", 5) == buf && !memcmp(buf, "ab\0\0\0z", 6));   /* padded */
    memset(buf, 'z', sizeof buf);
    CC(strncpy(buf, "abcdef", 3) == buf && !memcmp(buf, "abcz", 4));    /* no NUL */
    strcpy(buf, "ab");
    CC(strncat(buf, "cdef", 2) == buf && !strcmp(buf, "abcd"));
    CC(strncat(buf, "", 5) == buf && !strcmp(buf, "abcd"));
    CC(strncat(buf, "xy", 0) == buf && !strcmp(buf, "abcd"));
    {
        const char *s = "a/b/c";

        CC(strchr(s, '/') == s + 1 && strrchr(s, '/') == s + 3);
        CC(strchr(s, 0) == s + 5 && strrchr(s, 0) == s + 5);     /* the NUL */
        CC(!strchr(s, 'x') && !strrchr(s, 'x'));
        CC(strchr(s, 'a' + 256) == s);                          /* converted to char */
        CC(strstr(s, "b/c") == s + 2 && strstr(s, "") == s && !strstr(s, "c/") &&
           !strstr("", "a") && strstr("", "") != nullptr);
        CC(strstr(s, "/c") == s + 3 && strstr(s, "a/b/c/") == nullptr);
        CC(memchr(s, '/', 1) == nullptr && memchr(s, '/', 2) == s + 1 && memchr(s, 0, 6) == s + 5);
        CC(memchr("\xff", 0xff, 1) != nullptr && memchr("\xff", -1, 1) != nullptr);
    }
    memset(buf, 1, sizeof buf);
    CC(memset_explicit(buf, 0, 8) == buf && !buf[0] && !buf[7] && buf[8] == 1);
    return 0;
}

static unsigned cc_ctype(unsigned *cases)
{
    unsigned n[13] = { 0 };

    for (int c = -1; c < 256; c++) {
        int t[13] = { isalnum(c), isalpha(c), isblank(c), iscntrl(c), isdigit(c), isgraph(c),
                      islower(c), isprint(c), ispunct(c), isspace(c), isupper(c), isxdigit(c),
                      toupper(c) != c || tolower(c) != c };

        for (unsigned i = 0; i < 13; i++)
            n[i] += t[i] != 0;
        CC(c >= 0 && c < 0x80 ? true : !(t[0] | t[1] | t[2] | t[3] | t[5] | t[7] | t[12]));
        CC(!!t[0] == (t[1] || t[4]) && !!t[1] == (t[6] || t[10]));
        CC(!!t[5] == (t[7] && c != ' ') && !!t[8] == (t[5] && !t[0]));
        CC(!islower(c) || toupper(c) == c - 32);
        CC(!isupper(c) || tolower(c) == c + 32);
    }
    /* alnum alpha blank cntrl digit graph lower print punct space upper xdigit cased */
    CC(n[0] == 62 && n[1] == 52 && n[2] == 2 && n[3] == 33 && n[4] == 10 && n[5] == 94);
    CC(n[6] == 26 && n[7] == 95 && n[8] == 32 && n[9] == 6 && n[10] == 26 && n[11] == 22);
    CC(n[12] == 52);
    return 0;
}

struct cc_strto {
    const char *s;
    int base;
    uint64_t v;         /* strtoull's */
    int64_t sv;         /* strtoll's */
    uint8_t end;        /* *end - s */
    bool range, srange; /* ERANGE from strtoull, strtoll */
};

static const struct cc_strto cc_strtos[] = {
    { "0", 10, 0, 0, 1, false, false },
    { "  \t\n42xyz", 10, 42, 42, 6, false, false },
    { "-42", 10, (uint64_t)-42, -42, 3, false, false },
    { "+7", 0, 7, 7, 2, false, false },
    { "0x1F", 0, 31, 31, 4, false, false },
    { "0X1f", 16, 31, 31, 4, false, false },
    { "1f", 16, 31, 31, 2, false, false },
    { "0x", 0, 0, 0, 1, false, false },            /* "0", then 'x' isn't read */
    { "0xg", 16, 0, 0, 1, false, false },
    { "017", 0, 15, 15, 3, false, false },
    { "089", 0, 0, 0, 1, false, false },           /* octal: '8' ends it */
    { "0b101", 0, 5, 5, 5, false, false },          /* C23's */
    { "0b101", 2, 5, 5, 5, false, false },
    { "0b2", 0, 0, 0, 1, false, false },
    { "0b101", 16, 0xb101, 0xb101, 5, false, false },
    { "zZ", 36, 35 * 36 + 35, 35 * 36 + 35, 2, false, false },
    { "", 10, 0, 0, 0, false, false },
    { "   ", 10, 0, 0, 0, false, false },          /* no digits: end is s */
    { "-", 10, 0, 0, 0, false, false },
    { " +x", 16, 0, 0, 0, false, false },
    { "9223372036854775807", 10, INT64_MAX, INT64_MAX, 19, false, false },
    { "9223372036854775808", 10, (uint64_t)INT64_MAX + 1, INT64_MAX, 19, false, true },
    { "-9223372036854775808", 10, (uint64_t)INT64_MIN, INT64_MIN, 20, false, false },
    { "-9223372036854775809", 10, (uint64_t)INT64_MAX, INT64_MIN, 20, false, true },
    { "18446744073709551615", 10, UINT64_MAX, INT64_MAX, 20, false, true },
    { "18446744073709551616", 10, UINT64_MAX, INT64_MAX, 20, true, true },
    { "-18446744073709551615", 10, 1, INT64_MIN, 21, false, true },
    { "0xffffffffffffffffff", 0, UINT64_MAX, INT64_MAX, 20, true, true },
    { "99999999999999999999999999999x", 10, UINT64_MAX, INT64_MAX, 29, true, true },
};

static unsigned cc_strto(unsigned *cases)
{
    for (unsigned i = 0; i < sizeof cc_strtos / sizeof cc_strtos[0]; i++) {
        const struct cc_strto *t = &cc_strtos[i];
        char *end = nullptr;
        uint64_t v;
        int64_t sv;

        errno = 0;
        v = strtoull(t->s, &end, t->base);
        CC(v == t->v && end == t->s + t->end && (errno == ERANGE) == t->range);
        errno = 0;
        sv = strtoll(t->s, &end, t->base);
        CC(sv == t->sv && end == t->s + t->end && (errno == ERANGE) == t->srange);
        errno = 0;
        CC(strtoul(t->s, nullptr, t->base) == t->v && strtol(t->s, nullptr, t->base) == t->sv);
        CC(strtoumax(t->s, nullptr, t->base) == t->v && strtoimax(t->s, nullptr, t->base) == t->sv);
    }
    {
        char *end = nullptr;
        const char *s = "12";

        errno = 0;
        CC(strtoul(s, &end, 1) == 0 && end == s && errno == EINVAL);
        errno = 0;
        CC(strtoul(s, &end, 37) == 0 && end == s && errno == EINVAL);
        errno = 0;
        CC(strtoul(s, &end, -1) == 0 && end == s && errno == EINVAL);
    }
    CC(abs(-5) == 5 && labs(-5L) == 5 && llabs(-5LL) == 5 && imaxabs(-5) == 5 && abs(5) == 5);
    return 0;
}

static unsigned cc_memory(unsigned *cases)
{
    void *a, *b, *blocks[40];
    uint8_t *p;

    for (unsigned i = 0; i < 40; i++) {
        CC((blocks[i] = malloc(i * 7)) != nullptr);
        CC(memalignment(blocks[i]) >= 16);
        memset(blocks[i], (int)i, i * 7);
    }
    for (unsigned i = 0; i < 40; i++) {
        p = blocks[i];
        for (unsigned j = 0; j < i * 7; j++)
            CC(p[j] == i);
        free(blocks[i]);
    }
    CC((a = malloc(0)) && (b = malloc(0)) && a != b);   /* each a block of its own */
    free(a);
    free(b);
    free(nullptr);
    CC((p = calloc(100, 3)) != nullptr);
    for (unsigned j = 0; j < 300; j++)
        CC(p[j] == 0);
    free(p);
    errno = 0;
    CC(calloc(SIZE_MAX / 2, 3) == nullptr && errno == ENOMEM);    /* n * size overflows */
    errno = 0;
    CC(malloc(SIZE_MAX - 8) == nullptr && errno == ENOMEM);
    CC((p = malloc(1)) != nullptr);
    for (unsigned n = 1; n <= 300; n++) {               /* grown a byte at a time */
        CC((p = realloc(p, n)) != nullptr && _vec_size(p) >= n);
        p[n - 1] = (uint8_t)n;
        CC(n == 1 || p[n - 2] == (uint8_t)(n - 1));
    }
    free(p);
    CC((p = realloc(nullptr, 10)) != nullptr);
    for (unsigned j = 0; j < 10; j++)
        p[j] = (uint8_t)j;
    CC(realloc(p, 4) == p);                             /* shrunk: kept */
    for (unsigned n = 10; n < 200000; n = n * 3 + 1) {  /* past the pool's threshold */
        uint8_t *q = realloc(p, n);

        CC(q != nullptr && memalignment(q) >= 16);
        for (unsigned j = 0; j < 10; j++)
            CC(q[j] == j);
        memset(q + 10, 0xa5, n - 10);
        p = q;
    }
    free(p);
    CC(memalignment(nullptr) == 0 && memalignment((void *)0x30) == 16 &&
       memalignment((void *)0x1000) == 0x1000);
    return 0;
}

static int cc_cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    return (x > y) - (x < y);
}

/* 24-byte records, by their key, so a swap must move every byte. */
struct cc_rec {
    uint64_t key, check;
    uint8_t pad[8];
};

static int cc_cmp_rec(const void *a, const void *b)
{
    uint64_t x = ((const struct cc_rec *)a)->key, y = ((const struct cc_rec *)b)->key;

    return (x > y) - (x < y);
}

static int cc_cmp_byte(const void *a, const void *b)
{
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

static unsigned cc_sort(unsigned *cases)
{
    static int v[300];
    static struct cc_rec r[100];
    static uint8_t bytes[257];
    uint64_t seed = 0x2545f4914f6cdd1d;

    for (unsigned n = 0; n <= 300; n += n < 20 ? 1 : 37) {
        uint64_t sum = 0, after = 0;

        for (unsigned i = 0; i < n; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            v[i] = (int)(seed >> 33) % (n / 2 + 1) - (int)(n / 4);    /* with duplicates */
            sum += (uint64_t)(int64_t)v[i] * 2654435761u;
        }
        qsort(v, n, sizeof v[0], cc_cmp_int);
        for (unsigned i = 0; i < n; i++)
            after += (uint64_t)(int64_t)v[i] * 2654435761u;
        CC(sum == after);
        for (unsigned i = 1; i < n; i++)
            CC(v[i - 1] <= v[i]);
        for (unsigned i = 0; i < n; i++) {
            int *f = bsearch(&v[i], v, n, sizeof v[0], cc_cmp_int);

            CC(f && *f == v[i]);
        }
        {
            int absent = (int)n + 1;

            CC(!bsearch(&absent, v, n, sizeof v[0], cc_cmp_int));
        }
    }
    for (unsigned i = 0; i < 100; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        r[i] = (struct cc_rec){ seed >> 40, ~(seed >> 40), { (uint8_t)i } };
    }
    qsort(r, 100, sizeof r[0], cc_cmp_rec);
    for (unsigned i = 0; i < 100; i++)
        CC(r[i].check == ~r[i].key && (i == 0 || r[i - 1].key <= r[i].key));
    for (unsigned i = 0; i < 257; i++)
        bytes[i] = (uint8_t)(255 - i % 256);
    qsort(bytes, 257, 1, cc_cmp_byte);
    CC(bytes[0] == 0 && bytes[1] == 1 && bytes[254] == 254 && bytes[255] == 255 && bytes[256] == 255);
    qsort(nullptr, 0, 4, cc_cmp_int);
    CC(!bsearch(&v[0], nullptr, 0, sizeof v[0], cc_cmp_int));
    return 0;
}

static unsigned cc_stdio(unsigned *cases)
{
    char buf[8];

    CC(snprintf(buf, sizeof buf, "%d-%s", 42, "abc") == 6 && !strcmp(buf, "42-abc"));
    CC(snprintf(buf, sizeof buf, "%s", "abcdefghij") == 10 && !strcmp(buf, "abcdefg"));
    CC(snprintf(nullptr, 0, "%" PRIu64 "%" PRIx64, UINT64_MAX, (uint64_t)255) == 22);
    CC(snprintf(buf, sizeof buf, "%" PRId32 "%" PRIX8, -5, 0xab) == 4 && !strcmp(buf, "-5AB"));
    return 0;
}

/* Every unsigned char, and some of each wider type, against loops. */
static unsigned cc_bits(unsigned *cases)
{
    for (unsigned x = 0; x < 256; x++) {
        unsigned char c = (unsigned char)x;
        unsigned lz = 0, tz = 0, ones = 0;

        while (lz < 8 && !(c & (0x80 >> lz)))
            lz++;
        while (tz < 8 && !(c & (1u << tz)))
            tz++;
        for (unsigned b = 0; b < 8; b++)
            ones += (c >> b) & 1;
        CC(stdc_leading_zeros(c) == lz && stdc_trailing_zeros(c) == tz);
        CC(stdc_count_ones(c) == ones && stdc_count_zeros(c) == 8 - ones);
        CC(stdc_bit_width(c) == 8 - lz && stdc_has_single_bit(c) == (ones == 1));
        CC(stdc_first_leading_one(c) == (c ? lz + 1 : 0));
        CC(stdc_first_trailing_one(c) == (c ? tz + 1 : 0));
        CC(stdc_leading_ones(c) == stdc_leading_zeros((unsigned char)~c));
        CC(stdc_first_leading_zero(c) == (c == 0xff ? 0 : stdc_leading_ones(c) + 1));
        CC(stdc_first_trailing_zero(c) == (c == 0xff ? 0 : stdc_trailing_ones(c) + 1));
        CC(stdc_bit_floor(c) == (c ? 0x80u >> lz : 0));
        {
            unsigned ceil = 1;

            while (ceil < c)
                ceil <<= 1;
            CC(stdc_bit_ceil(c) == (ceil > 0xff ? 0 : ceil));
        }
    }
    CC(stdc_leading_zeros(0u) == 32 && stdc_leading_zeros(0ul) == 64 &&
       stdc_leading_zeros(0ull) == 64 && stdc_leading_zeros((unsigned short)0) == 16);
    CC(stdc_trailing_zeros(1ull << 63) == 63 && stdc_count_ones(~0ull) == 64);
    CC(stdc_bit_ceil(5ul) == 8 && stdc_bit_ceil((1ul << 63) + 1) == 0 && stdc_bit_ceil(0u) == 1);
    CC(stdc_bit_floor(~0ull) == 1ull << 63 && stdc_bit_width(~0ull) == 64);
    CC(stdc_leading_ones(~0u) == 32 && stdc_first_leading_zero(~0u) == 0);
    return 0;
}

static unsigned clib_check(unsigned *cases)
{
    unsigned line;

    if ((line = cc_strings(cases)) || (line = cc_ctype(cases)) || (line = cc_strto(cases)) ||
        (line = cc_memory(cases)) || (line = cc_sort(cases)) || (line = cc_stdio(cases)) ||
        (line = cc_bits(cases)))
        return line;
    return 0;
}

#undef CC

#endif /* CONFORMANCE_CLIBCHECK_H */
