/*
 * tools/span-fuzz/fuzz.c -- utility.library's checked readers, fuzzed
 *
 * libFuzzer, with ASan and UBSan. Each input is an offset, a length and
 * which call, then the bytes the span covers, copied to a block of
 * exactly that size, so a read one byte past it is ASan's to catch. Each
 * call is checked against a model whose bounds are 128-bit, which can't
 * wrap: it must succeed exactly when the bytes are inside, and then load
 * what's there. StrToU64 is checked against a 128-bit parser the same
 * way, and StrSplit and StrTrim against what they must leave.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/utility.h>

static uint64_t get64(const uint8_t *p)
{
    uint64_t v;

    memcpy(&v, p, 8);
    return v;
}

/* Small offsets most of the time, so the edges are hit; any, otherwise. */
static uint64_t shape(uint64_t raw, uint64_t len, uint8_t how)
{
    switch (how & 3) {
    case 0: return raw % (len + 16);
    case 1: return UINT64_MAX - raw % 16;
    case 2: return len - raw % 16;          /* may wrap: as large as it gets */
    }
    return raw;
}

static void check_at(bool cond, int line)
{
    if (!cond) {
        fprintf(stderr, "span-fuzz: fuzz.c:%d doesn't hold\n", line);
        abort();
    }
}

#define check(cond) check_at(cond, __LINE__)

static void readers(struct Span s, uint64_t off, uint64_t len, uint8_t op)
{
    static const unsigned sizes[] = { 1, 2, 4, 8, 2, 4, 8 };
    unsigned n = sizes[op % 7];
    bool inside = (unsigned __int128)off + n <= s.sp_Len, ok;
    uint64_t want = 0, got = 0, untouched = 0;
    struct Span sub = { nullptr, 7 };

    if (inside)
        for (unsigned i = 0; i < n; i++)
            want |= (uint64_t)s.sp_Data[off + (op % 7 >= 4 ? n - 1 - i : i)] << (8 * i);
#define TRY(fn, type) \
    { \
        type v = (type)0x5a5a5a5a5a5a5a5a; \
\
        ok = fn(s, off, &v); \
        got = v; \
        untouched = (type)0x5a5a5a5a5a5a5a5a; \
        break; \
    }
    switch (op % 7) {
    case 0: TRY(SpanReadU8, uint8_t)
    case 1: TRY(SpanReadU16LE, uint16_t)
    case 2: TRY(SpanReadU32LE, uint32_t)
    case 3: TRY(SpanReadU64LE, uint64_t)
    case 4: TRY(SpanReadU16BE, uint16_t)
    case 5: TRY(SpanReadU32BE, uint32_t)
    default: TRY(SpanReadU64BE, uint64_t)
    }
#undef TRY
    check(ok == inside);
    check(got == (ok ? want : untouched));      /* refused: *out untouched */

    inside = (unsigned __int128)off + len <= s.sp_Len;
    check(SpanSlice(s, off, len, &sub) == inside);
    if (inside)
        check(sub.sp_Data == s.sp_Data + off && sub.sp_Len == len);
    else
        check(!sub.sp_Data && sub.sp_Len == 7);
}

static void strings(struct Str s, uint8_t op)
{
    unsigned base = op % 38;                /* 0, 1 and 37 among them */
    unsigned __int128 v = 0;
    uint64_t got = 99;
    bool ok = true;
    struct Str t = s, head, tail;
    unsigned b = base;

    if (b == 0) {
        b = 10;
        if (t.st_Len > 2 && t.st_Data[0] == '0') {
            char c = t.st_Data[1] | 0x20;

            b = c == 'x' ? 16 : c == 'b' ? 2 : c == 'o' ? 8 : 10;
            if (b != 10)
                t = (struct Str){ t.st_Data + 2, t.st_Len - 2 };
        }
    }
    if (b < 2 || b > 36 || !t.st_Len)
        ok = false;
    for (uint64_t i = 0; ok && i < t.st_Len; i++) {
        char c = t.st_Data[i];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                   : c >= 'a' && c <= 'z' ? (unsigned)(c - 'a' + 10)
                   : c >= 'A' && c <= 'Z' ? (unsigned)(c - 'A' + 10) : 99;

        v = v * b + d;
        ok = d < b && v <= UINT64_MAX;
    }
    check(StrToU64(s, base, &got) == ok);
    check(ok ? got == (uint64_t)v : got == 99);

    if (StrSplit(s, (char)op, &head, &tail)) {
        check(head.st_Data == s.st_Data && head.st_Len < s.st_Len);
        check(s.st_Data[head.st_Len] == (char)op && !memchr(head.st_Data, (char)op, head.st_Len));
        check(tail.st_Data == s.st_Data + head.st_Len + 1 &&
              head.st_Len + 1 + tail.st_Len == s.st_Len);
    } else
        check(head.st_Len == s.st_Len && !tail.st_Len && !memchr(s.st_Data, (char)op, s.st_Len));
    t = StrTrim(s);
    check(t.st_Data >= s.st_Data && t.st_Data + t.st_Len <= s.st_Data + s.st_Len);
    if (t.st_Len)
        check(!memchr(" \t\r\n", t.st_Data[0], 4) && !memchr(" \t\r\n", t.st_Data[t.st_Len - 1], 4));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *copy;
    uint64_t len;

    if (size < 18)
        return 0;
    len = size - 18;
    if (!(copy = malloc(len ? len : 1)))
        return 0;
    memcpy(copy, data + 18, len);
    readers((struct Span){ copy, len }, shape(get64(data), len, data[16]),
            shape(get64(data + 8), len, data[16] >> 2), data[17]);
    strings((struct Str){ (const char *)copy, len }, data[17]);
    free(copy);
    return 0;
}
