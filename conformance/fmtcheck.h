/*
 * conformance/fmtcheck.h -- RawDoFmt against known strings
 *
 * Each case a format, its arguments and what it must write, with the
 * count RawDoFmt returns and the one NUL it ends with. Shared by
 * conformance/pool.c, in the guest, and lib/exec/test.c, on the host.
 */
#ifndef CONFORMANCE_FMTCHECK_H
#define CONFORMANCE_FMTCHECK_H

#include <planar/exec.h>
#include <planar/utility.h>

struct fc_buf {
    char text[128];
    unsigned len, nuls;
};

static void fc_put(char c, void *data)
{
    struct fc_buf *b = data;

    if (!c)
        b->nuls++;
    else if (b->len < sizeof b->text - 1)
        b->text[b->len++] = c;
}

/* What failed, for the caller to say. */
struct fmtcheck_fail {
    unsigned line;
    const char *want;
    char got[128];
};

static bool fc_case(struct fmtcheck_fail *f, unsigned line, const char *want, const char *fmt, ...)
{
    struct fc_buf b = { 0 };
    uint64_t n, i;
    va_list ap;

    va_start(ap, fmt);
    n = RawDoFmt(fmt, ap, fc_put, &b);
    va_end(ap);
    b.text[b.len] = '\0';
    for (i = 0; want[i] && want[i] == b.text[i]; i++)
        ;
    if (!want[i] && !b.text[i] && n == b.len && b.nuls == 1)
        return true;
    f->line = line;
    f->want = want;
    for (i = 0; i <= b.len; i++)
        f->got[i] = b.text[i];
    return false;
}

#define FC(want, ...) \
    if (!fc_case(f, __LINE__, want, __VA_ARGS__)) \
        return 1; \
    (*cases)++

/* 0 if all held; else which didn't, in '*f'. */
static int fmt_check(struct fmtcheck_fail *f, unsigned *cases)
{
    FC("plain", "plain");
    FC("", "");
    FC("0 -42 2147483647 -2147483648", "%d %i %d %d", 0, -42, 2147483647, (int)-2147483648LL);
    FC("-9223372036854775808", "%ld", (long)(-9223372036854775807LL - 1));
    FC("18446744073709551615 4294967295", "%llu %u", ~0ULL, ~0u);
    FC("deadbeef DEADBEEF 0xff 0XFF", "%x %X %#x %#X", 0xdeadbeefu, 0xdeadbeefu, 255u, 255u);
    FC("0", "%#x", 0u);                             /* no 0x for 0 */
    FC("0x0123456789abcdef", "%#018lx", 0x0123456789abcdefUL);
    FC("0000002a|42    |    42", "%08x|%-6d|%6d", 42u, 42, 42);
    FC("+5 -5  5", "%+d %+d % d", 5, -5, 5);
    FC("007|  007|-007|", "%.3d|%5.3d|%.3d|", 7, 7, -7);
    FC("||    ", "%.0d|%.0u|%4.0x", 0, 0u, 0u);    /* precision 0 writes no 0 */
    FC("   42", "%05.0d", 42);                      /* a precision drops '0' */
    FC("-0042", "%05d", -42);
    FC("10 010 0", "%o %#o %#o", 8u, 8u, 0u);
    FC("101 0b101 0B101", "%b %#b %#B", 5u, 5u, 5u);
    FC("44 4464 255 65535", "%hhd %hd %hhu %hu", 300, 70000, -1, -1);
    FC("1 -1 4294967295", "%w8u %w16d %w32u", 257u, 0xffff, -1);
    FC("ffffffffffffffff -3", "%w64x %wf32d", ~0ULL, -3);
    FC("123456789012 -7 9", "%zu %td %jd", (size_t)123456789012ULL, (ptrdiff_t)-7, (intmax_t)9);
    FC("x|  y|z  |", "%c|%3c|%-3c|", 'x', 'y', 'z');
    FC("  abc|abc  |ab|(null)", "%5s|%-5s|%.2s|%s", "abc", "abc", "abc", (const char *)nullptr);
    FC("hello, hel|  wor", "%S, %.3S|%5.3S", STR("hello"), STR("hello"), STR("world"));
    FC("[ab]", "[%S]", (struct Str){ "abcdef", 2 });    /* by its length, not a NUL */
    FC("   42|42   |", "%*d|%*d|", 5, 42, -5, 42);
    FC("abc  |", "%-*.*s|", 5, 3, "abcdef");
    FC("100% %", "100%% %%");
    FC("%q %y5", "%q %y5");                         /* unknown: as it stands */
    FC("ends %", "ends %");
    FC("ends %-08", "ends %-08");
    FC("0x1000 0x0", "%p %p", (void *)0x1000, (void *)nullptr);
    return 0;
}

#undef FC

#endif /* CONFORMANCE_FMTCHECK_H */
