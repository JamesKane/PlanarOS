/*
 * lib/clib/strto.c -- C's strtol family (docs §17.4.5)
 *
 * As C23 reads a number: spaces, a sign, then digits in 'base', with
 * 0x for 16, 0b for 2 (C23's), and base 0 taking either, a leading 0
 * for 8, or else 10. Past the type's range it's ERANGE and the limit;
 * a negative number given to the unsigned ones is negated, as C says.
 * With no digits, *end is 's' and the result 0.
 */
#include <stdckdint.h>
#include <stdint.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>

static_assert(sizeof(long) == sizeof(long long) && sizeof(long long) == sizeof(uint64_t));

static unsigned digit(char c)
{
    if (isdigit(c))
        return (unsigned)(c - '0');
    if (isalpha(c))
        return (unsigned)(tolower(c) - 'a' + 10);
    return 36;
}

/* The magnitude; *neg if there was a '-', *over if it passed 64 bits. */
static uint64_t scan(const char *s, char **end, int base, bool *neg, bool *over)
{
    const char *p = s;
    uint64_t v = 0;
    bool any = false;

    *neg = *over = false;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (end)
            *end = (char *)s;
        return 0;
    }
    while (isspace(*p))
        p++;
    if (*p == '+' || *p == '-')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && tolower(p[1]) == 'x' && isxdigit(p[2])) {
        p += 2;
        base = 16;
    } else if ((base == 0 || base == 2) && p[0] == '0' && tolower(p[1]) == 'b' &&
               (p[2] == '0' || p[2] == '1')) {
        p += 2;
        base = 2;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    for (unsigned d; (d = digit(*p)) < (unsigned)base; p++) {
        any = true;
        if (ckd_mul(&v, v, (uint64_t)base) || ckd_add(&v, v, d))
            *over = true;
    }
    if (end)
        *end = (char *)(any ? p : s);
    return v;
}

static int64_t to_signed(const char *s, char **end, int base)
{
    bool neg, over;
    uint64_t v = scan(s, end, base, &neg, &over);

    if (over || v > (uint64_t)INT64_MAX + neg) {
        errno = ERANGE;
        return neg ? INT64_MIN : INT64_MAX;
    }
    return neg ? (int64_t)(0 - v) : (int64_t)v;
}

static uint64_t to_unsigned(const char *s, char **end, int base)
{
    bool neg, over;
    uint64_t v = scan(s, end, base, &neg, &over);

    if (over) {
        errno = ERANGE;
        return UINT64_MAX;
    }
    return neg ? 0 - v : v;
}

long strtol(const char *restrict s, char **restrict end, int base)
{
    return to_signed(s, end, base);
}

long long strtoll(const char *restrict s, char **restrict end, int base)
{
    return to_signed(s, end, base);
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base)
{
    return to_unsigned(s, end, base);
}

unsigned long long strtoull(const char *restrict s, char **restrict end, int base)
{
    return to_unsigned(s, end, base);
}

intmax_t strtoimax(const char *restrict s, char **restrict end, int base)
{
    return to_signed(s, end, base);
}

uintmax_t strtoumax(const char *restrict s, char **restrict end, int base)
{
    return to_unsigned(s, end, base);
}

intmax_t imaxabs(intmax_t j)
{
    return j < 0 ? -j : j;
}
