/*
 * conformance/int128.c -- 128-bit division, modulo and multiplication
 * (V6.1)
 *
 * compiler-rt's builtins (third_party/compiler-rt) against the
 * compiler's own arithmetic: each case's answers are folded at compile
 * time, and its operands read back through a volatile at run time, so
 * the program calls __udivti3 and the rest for them. Then random cases,
 * which only the identities check: a = q*b + r, with r below b. Exits 0,
 * or the number of the first case that failed.
 */
#include <planar/exec.h>

#include "log.h"

typedef unsigned __int128 u128;
typedef __int128 s128;

#define U(hi, lo)   ((u128)(hi) << 64 | (lo))

struct ucase {
    u128 a, b, q, r, m;
};
struct scase {
    s128 a, b, q, r;
};

#define UC(x, y)    { (x), (y), (x) / (y), (x) % (y), (x) * (y) }
#define SC(x, y)    { (x), (y), (x) / (y), (x) % (y) }

static const struct ucase ucases[] = {
    UC(U(0, 7), U(0, 2)),
    UC(U(1, 0), U(0, 3)),                       /* past 64 bits, by a small one */
    UC(U(0x0123456789abcdef, 0xfedcba9876543210), U(0, 0x100000001)),
    UC(U(~0ULL, ~0ULL), U(0, 1)),
    UC(U(~0ULL, ~0ULL), U(~0ULL, ~0ULL)),
    UC(U(~0ULL, ~0ULL), U(1, 0)),               /* by 2^64 */
    UC(U(0x8000000000000000, 0), U(0x4000000000000000, 1)),
    UC(U(0xdeadbeefcafef00d, 0x0123456789abcdef), U(0x00000000ffffffff, 0xffffffff00000000)),
    UC(U(0, 0x1234), U(5, 0)),                  /* a below b: q is 0 */
    UC(U(0x7fffffffffffffff, 0xffffffffffffffff), U(0, 0xfffffffffffffffb)),
    UC(U(0x0000000100000000, 0), U(0, 0x00000000ffffffff)),
};

static const struct scase scases[] = {
    SC((s128)-7, (s128)2),
    SC((s128)7, (s128)-2),
    SC(-(s128)U(0x0123456789abcdef, 0xfedcba9876543210), (s128)U(0, 0x100000001)),
    SC((s128)U(0x7fffffffffffffff, ~0ULL), (s128)-3),
    SC(-(s128)U(0x7fffffffffffffff, ~0ULL) - 1, (s128)7),   /* the most negative */
    SC((s128)U(0, 5), (s128)U(0, 5)),
};

static volatile u128 sink;

/* What the program can't fold: v through a volatile. */
static u128 opaque(u128 v)
{
    sink = v;
    return sink;
}

/* xorshift128+, for the random cases. */
static uint64_t s0 = 0x9e3779b97f4a7c15, s1 = 0xbf58476d1ce4e5b9;

static uint64_t rnd(void)
{
    uint64_t x = s0, y = s1;

    s0 = y;
    x ^= x << 23;
    s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return s1 + y;
}

int main(void)
{
    int n = 0;
    unsigned random = 0;

    for (unsigned i = 0; i < sizeof ucases / sizeof ucases[0]; i++) {
        const struct ucase *c = &ucases[i];
        u128 a = opaque(c->a), d = opaque(c->b);

        n++;
        if (a / d != c->q || a % d != c->r || a * d != c->m)
            return n;
    }
    for (unsigned i = 0; i < sizeof scases / sizeof scases[0]; i++) {
        const struct scase *c = &scases[i];
        s128 a = (s128)opaque((u128)c->a), d = (s128)opaque((u128)c->b);

        n++;
        if (a / d != c->q || a % d != c->r)
            return n;
    }
    /* _BitInt(128) goes the same way. */
    {
        unsigned _BitInt(128) a = (unsigned _BitInt(128))opaque(U(0xfedcba9876543210, 7));
        unsigned _BitInt(128) d = (unsigned _BitInt(128))opaque(U(0, 1000000007));

        n++;
        if ((u128)(a / d) != U(0xfedcba9876543210, 7) / U(0, 1000000007))
            return n;
    }
    /* Divisors of every width, from 1 bit to 128. */
    for (unsigned i = 0; i < 4096; i++, random++) {
        unsigned bits = 1 + i % 128;
        u128 a = U(rnd(), rnd()), d = U(rnd(), rnd()) >> (128 - bits), q, r;

        if (!d)
            d = 1;
        q = a / d;
        r = a % d;
        if (r >= d || q * d + r != a)
            return 1000 + (int)i;
    }
    KPrintF("int128: %lu cases as the compiler folds them, %lu random ones by their identities",
            (uint64_t)n, (uint64_t)random);
    return 0;
}
