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
static void log_rate(struct logbuf *b, uint64_t bytes, uint64_t ns)
{
    uint64_t t = ns ? bytes * 10000 / ns : 0;

    log_num(b, t / 10);
    log_str(b, ".");
    log_num(b, t % 10);
}

int main(void)
{
    struct memcheck_fail f;
    struct logbuf b = { 0 };
    uint64_t cases = 0, t0, copy, set, bytes;

    if (memcheck(&f, &cases)) {
        log_str(&b, "memfns: ");
        log_str(&b, f.fn);
        log_str(&b, " wrong at length ");
        log_num(&b, f.len);
        log_str(&b, ", source +");
        log_num(&b, f.salign);
        log_str(&b, ", destination +");
        log_num(&b, f.dalign);
        KLog(b.text);
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

    log_str(&b, "memfns: ");
    log_num(&b, cases);
    log_str(&b, " cases right; 64 KiB copied at ");
    log_rate(&b, (uint64_t)BIG * ROUNDS, copy);
    log_str(&b, " bytes/us, set at ");
    log_rate(&b, (uint64_t)BIG * ROUNDS, set);
    log_str(&b, ", a byte loop ");
    log_rate(&b, (uint64_t)BIG * ROUNDS, bytes);
    KLog(b.text);
    return 0;
}
