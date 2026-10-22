/*
 * conformance/memfns.c -- the ISA's memcpy, memmove, memset and memcmp
 * (V6.1)
 *
 * conformance/memcheck.h's cases on the functions every program links:
 * RVV or NEON for the first three (arch/<isa>/user/string.S), words for
 * memcmp (lib/rt/string.c). Then what they cost: 64 KiB copied and set,
 * against a byte loop. Exits 0, or 1 with what failed logged.
 */
#include <planar/exec.h>

#include "log.h"
#include "memcheck.h"

constexpr unsigned BIG = 64 * 1024;
constexpr unsigned ROUNDS = 16;

static uint8_t big_src[BIG] __attribute__((aligned(64)));
static uint8_t big_dst[BIG] __attribute__((aligned(64)));

static void byte_copy(volatile uint8_t *d, const uint8_t *s, unsigned n)
{
    while (n--)
        *d++ = *s++;
}

/* Bytes a microsecond, in tenths. */
static void fmt_rate(char *out, size_t size, uint64_t bytes, uint64_t ns)
{
    uint64_t t = ns ? bytes * 10000 / ns : 0;

    SNPrintf(out, size, "%lu.%lu", t / 10, t % 10);
}

int main(void)
{
    struct memcheck_fail f;
    char copy_s[32], set_s[32], bytes_s[32];
    uint64_t cases = 0, t0, copy, set, bytes;

    if (memcheck(&f, &cases)) {
        KPrintF("memfns: %s wrong at length %lu, source +%lu, destination +%lu",
                f.fn, (uint64_t)f.len, (uint64_t)f.salign, (uint64_t)f.dalign);
        return 1;
    }

    for (unsigned i = 0; i < BIG; i++)
        big_src[i] = (uint8_t)i;
    t0 = CurrentTime();
    for (unsigned r = 0; r < ROUNDS; r++)
        memcpy(big_dst, big_src, BIG);
    copy = CurrentTime() - t0;
    t0 = CurrentTime();
    for (unsigned r = 0; r < ROUNDS; r++)
        memset(big_dst, (int)r, BIG);
    set = CurrentTime() - t0;
    t0 = CurrentTime();
    byte_copy(big_dst, big_src, BIG);
    bytes = (CurrentTime() - t0) * ROUNDS;
    if (memcmp(big_dst, big_src, BIG) != 0)
        return 1;

    fmt_rate(copy_s, sizeof copy_s, (uint64_t)BIG * ROUNDS, copy);
    fmt_rate(set_s, sizeof set_s, (uint64_t)BIG * ROUNDS, set);
    fmt_rate(bytes_s, sizeof bytes_s, (uint64_t)BIG * ROUNDS, bytes);
    KPrintF("memfns: %lu cases right; 64 KiB copied at %s bytes/us, set at %s, a byte loop %s",
            cases, copy_s, set_s, bytes_s);
    return 0;
}
