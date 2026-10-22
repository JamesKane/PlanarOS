/*
 * conformance/memcheck.h -- memcpy, memmove, memset and memcmp against
 * byte loops
 *
 * Every length from 0 to MEMCHECK_MAX, at every alignment of source and
 * destination within 8 bytes, between guard bytes that must stay put.
 * memmove both ways over its own buffer, by every overlap. Shared by
 * conformance/memfns.c, on each ISA's own (arch/<isa>/user/string.S),
 * and lib/rt/test.c, on the host with lib/rt/string.c's word loops
 * renamed: MEMCHECK_PREFIX names the functions under test.
 */
#ifndef CONFORMANCE_MEMCHECK_H
#define CONFORMANCE_MEMCHECK_H

#include <stddef.h>
#include <stdint.h>

#ifndef MEMCHECK_PREFIX
#define MEMCHECK_PREFIX
#endif
#define MEMCHECK_CAT2(a, b) a##b
#define MEMCHECK_CAT(a, b)  MEMCHECK_CAT2(a, b)
#define T_MEMCPY    MEMCHECK_CAT(MEMCHECK_PREFIX, memcpy)
#define T_MEMMOVE   MEMCHECK_CAT(MEMCHECK_PREFIX, memmove)
#define T_MEMSET    MEMCHECK_CAT(MEMCHECK_PREFIX, memset)
#define T_MEMCMP    MEMCHECK_CAT(MEMCHECK_PREFIX, memcmp)

void *T_MEMCPY(void *restrict dst, const void *restrict src, size_t n);
void *T_MEMMOVE(void *dst, const void *src, size_t n);
void *T_MEMSET(void *dst, int c, size_t n);
int T_MEMCMP(const void *a, const void *b, size_t n);

constexpr unsigned MEMCHECK_MAX = 512;
constexpr unsigned MEMCHECK_GUARD = 64;     /* each side: wider than any one step's reach */
constexpr unsigned MEMCHECK_SHIFT = 72;     /* memmove's overlaps, either way */
constexpr unsigned MEMCHECK_BUF = 2 * MEMCHECK_GUARD + 8 + 2 * MEMCHECK_SHIFT + MEMCHECK_MAX;

static uint8_t mc_src[MEMCHECK_BUF] __attribute__((aligned(64)));
static uint8_t mc_dst[MEMCHECK_BUF] __attribute__((aligned(64)));
static uint8_t mc_want[MEMCHECK_BUF] __attribute__((aligned(64)));

/* What failed, for the caller to say. */
struct memcheck_fail {
    const char *fn;
    unsigned len, salign, dalign;
};

static uint32_t mc_seed = 0x2545f491;

static uint8_t mc_byte(void)
{
    mc_seed = mc_seed * 1664525 + 1013904223;
    return (uint8_t)(mc_seed >> 24);
}

/* Bytes that differ from their neighbours and from the last fill's. */
static void mc_fill(uint8_t *p, unsigned n)
{
    uint8_t k = mc_byte(), step = (uint8_t)(mc_byte() | 1);

    for (unsigned i = 0; i < n; i++, k += step)
        p[i] = k;
}

static bool mc_same(const uint8_t *p, const uint8_t *q, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (p[i] != q[i])
            return false;
    return true;
}

static int mc_sign(int v)
{
    return (v > 0) - (v < 0);
}

/* 0 if all held; else what didn't, in '*f'. 'cases' counts calls. */
static int memcheck(struct memcheck_fail *f, uint64_t *cases)
{
    for (unsigned len = 0; len <= MEMCHECK_MAX; len++)
        for (unsigned sa = 0; sa < 8; sa++)
            for (unsigned da = 0; da < 8; da++) {
                uint8_t *s = mc_src + MEMCHECK_GUARD + sa, *d = mc_dst + MEMCHECK_GUARD + da;
                uint8_t c = mc_byte();
                unsigned w = 2 * MEMCHECK_GUARD + 8 + len;  /* the window, guards and all */

                *f = (struct memcheck_fail){ "memcpy", len, sa, da };
                mc_fill(mc_src, w);
                mc_fill(mc_dst, w);
                for (unsigned i = 0; i < w; i++)
                    mc_want[i] = mc_dst[i];
                for (unsigned i = 0; i < len; i++)
                    mc_want[MEMCHECK_GUARD + da + i] = s[i];
                if (T_MEMCPY(d, s, len) != d || !mc_same(mc_dst, mc_want, w))
                    return 1;

                /* memcmp: equal, then one byte apart, either way. */
                f->fn = "memcmp";
                if (T_MEMCMP(d, s, len) != 0)
                    return 1;
                if (len) {
                    unsigned at = mc_byte() % len;
                    int want;

                    d[at] ^= (uint8_t)(1 + mc_byte() % 255);
                    want = d[at] < s[at] ? -1 : 1;
                    if (mc_sign(T_MEMCMP(d, s, len)) != want || mc_sign(T_MEMCMP(s, d, len)) != -want)
                        return 1;
                }

                f->fn = "memset";
                for (unsigned i = 0; i < w; i++)
                    mc_want[i] = mc_dst[i];
                for (unsigned i = 0; i < len; i++)
                    mc_want[MEMCHECK_GUARD + da + i] = c;
                if (T_MEMSET(d, c, len) != d || !mc_same(mc_dst, mc_want, w))
                    return 1;
                *cases += 4;
            }

    /* memmove within one buffer: the destination 'shift' bytes up or
     * down from the source, every shift a step could straddle. */
    for (unsigned len = 0; len <= MEMCHECK_MAX; len += 1 + len / 32)
        for (int shift = -(int)MEMCHECK_SHIFT; shift <= (int)MEMCHECK_SHIFT; shift++)
            for (unsigned sa = 0; sa < 8; sa++) {
                uint8_t *s = mc_dst + MEMCHECK_GUARD + MEMCHECK_SHIFT + sa, *d = s + shift;

                /* dalign: the shift, biased to be positive. */
                *f = (struct memcheck_fail){ "memmove", len, sa, (unsigned)(shift + (int)MEMCHECK_SHIFT) };
                mc_fill(mc_dst, MEMCHECK_BUF);
                for (unsigned i = 0; i < MEMCHECK_BUF; i++)
                    mc_want[i] = mc_dst[i];
                for (unsigned i = 0; i < len; i++)
                    mc_want[d - mc_dst + i] = s[i];
                if (T_MEMMOVE(d, s, len) != d || !mc_same(mc_dst, mc_want, MEMCHECK_BUF))
                    return 1;
                *cases += 1;
            }
    return 0;
}

#endif /* CONFORMANCE_MEMCHECK_H */
