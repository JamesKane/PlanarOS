/*
 * lib/clib/testhost.c -- lib/clib against the host's C library
 *
 * Built with the host's headers, beside lib/clib/test.c: random strings
 * through both strtol families, and random strings and arrays through
 * the string functions and qsort, which must agree. Strings that start
 * 0b are left out where the base takes C23's prefix, which the host's
 * library may not know.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

unsigned long long clib_strtoull(const char *s, char **end, int base);
long long clib_strtoll(const char *s, char **end, int base);
size_t clib_strlen(const char *s);
int clib_strcmp(const char *a, const char *b);
int clib_strncmp(const char *a, const char *b, size_t n);
char *clib_strchr(const char *s, int c);
char *clib_strrchr(const char *s, int c);
char *clib_strstr(const char *haystack, const char *needle);
void clib_qsort(void *base, size_t n, size_t size, int (*compar)(const void *, const void *));
extern _Thread_local int clib_errno;

unsigned clib_vs_host(unsigned *cases);

#define CH(cond) \
    if (!(cond)) \
        return __LINE__; \
    (*cases)++

static uint64_t seed = 0x9e3779b97f4a7c15;

static uint64_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

/* A string of up to 'max' bytes from 'from'. */
static void random_string(char *s, unsigned max, const char *from)
{
    unsigned n = (unsigned)(rnd() % (max + 1)), k = (unsigned)strlen(from);

    for (unsigned i = 0; i < n; i++)
        s[i] = from[rnd() % k];
    s[n] = 0;
}

static bool c23_prefix(const char *s, int base)
{
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '+' || *s == '-')
        s++;
    return (base == 0 || base == 2) && s[0] == '0' && (s[1] == 'b' || s[1] == 'B');
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    return (x > y) - (x < y);
}

static int sign(int v)
{
    return (v > 0) - (v < 0);
}

unsigned clib_vs_host(unsigned *cases)
{
    static const int bases[] = { 0, 2, 8, 10, 16, 36, 3, 7, 25 };
    char s[40], t[40];

    for (unsigned i = 0; i < 200000; i++) {
        int base = bases[i % (sizeof bases / sizeof bases[0])];
        char *end, *hend;

        random_string(s, i % 2 ? 30 : 8, i % 3 ? " \t+-0123456789abcdefxXbBzZ" : "0123456789fF");
        if (c23_prefix(s, base))
            continue;
        errno = clib_errno = 0;
        {
            unsigned long long a = clib_strtoull(s, &end, base), b = strtoull(s, &hend, base);

            CH(a == b && end == hend && (clib_errno == ERANGE) == (errno == ERANGE));
        }
        errno = clib_errno = 0;
        {
            long long a = clib_strtoll(s, &end, base), b = strtoll(s, &hend, base);

            CH(a == b && end == hend && (clib_errno == ERANGE) == (errno == ERANGE));
        }
    }
    for (unsigned i = 0; i < 100000; i++) {
        int c = "abc\x80/"[rnd() % 5];

        random_string(s, 12, "abc\x80/");
        random_string(t, 4, "abc\x80/");
        CH(clib_strlen(s) == strlen(s));
        CH(sign(clib_strcmp(s, t)) == sign(strcmp(s, t)));
        CH(sign(clib_strncmp(s, t, i % 6)) == sign(strncmp(s, t, i % 6)));
        CH(clib_strchr(s, c) == strchr(s, c) && clib_strrchr(s, c) == strrchr(s, c));
        CH(clib_strstr(s, t) == strstr(s, t));
    }
    for (unsigned i = 0; i < 2000; i++) {
        int a[64], b[64];
        unsigned n = (unsigned)(rnd() % 65);

        for (unsigned j = 0; j < n; j++)
            a[j] = b[j] = (int)(rnd() % 50) - 25;
        clib_qsort(a, n, sizeof a[0], cmp_int);
        qsort(b, n, sizeof b[0], cmp_int);
        CH(!memcmp(a, b, n * sizeof a[0]));
    }
    return 0;
}

#undef CH
