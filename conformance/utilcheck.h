/*
 * conformance/utilcheck.h -- utility.library against what it must do
 *
 * Tag lists through every control tag, hooks, case for all 256 bytes,
 * the comparisons' signs, SNPrintf cutting to fit, Str's helpers and
 * StrToU64's edges, and the Span readers at, and past, the end. Shared
 * by conformance/utility.c, in the guest, and lib/utility/test.c, on
 * the host with the sanitizers.
 */
#ifndef CONFORMANCE_UTILCHECK_H
#define CONFORMANCE_UTILCHECK_H

#include <planar/utility.h>

#define UC(cond) \
    if (!(cond)) \
        return __LINE__; \
    (*cases)++

static uint64_t uc_hook_entry(struct Hook *h, void *object, void *message)
{
    return (uint64_t)(uintptr_t)h->h_Data + (uint64_t)(uintptr_t)object * 3 +
           *(const uint64_t *)message;
}

static bool uc_same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static unsigned uc_tags(unsigned *cases)
{
    static const struct TagItem tail[] = {
        { TAG_USER + 5, 50 }, { TAG_USER + 1, 99 }, { TAG_DONE, 0 }, { TAG_USER + 6, 60 },
    };
    const struct TagItem head[] = {
        { TAG_USER + 1, 10 }, { TAG_IGNORE, 0 }, { TAG_USER + 2, 20 },
        { TAG_SKIP, 2 }, { TAG_USER + 3, 30 }, { TAG_USER + 4, 40 },
        { TAG_USER + 7, 70 }, { TAG_MORE, (uint64_t)(uintptr_t)tail },
        { TAG_USER + 8, 80 },                   /* after TAG_MORE: never seen */
    };
    static const Tag want[] = { TAG_USER + 1, TAG_USER + 2, TAG_USER + 7, TAG_USER + 5,
                                TAG_USER + 1 };
    static const Tag array[] = { 7, 9, TAG_USER, TAG_DONE, 11 };
    const struct TagItem *walk = head;
    const struct TagItem dead_end[] = { { TAG_USER, 1 }, { TAG_MORE, 0 } };
    struct TagItem *t;
    unsigned n = 0;

    while ((t = NextTagItem(&walk))) {
        UC(n < sizeof want / sizeof want[0] && t->ti_Tag == want[n]);
        n++;
    }
    UC(n == 5 && !walk && !NextTagItem(&walk) && !NextTagItem(nullptr));
    UC(FindTagItem(TAG_USER + 1, head) == &head[0]);       /* the first of two */
    UC(FindTagItem(TAG_USER + 5, head) == &tail[0]);
    UC(!FindTagItem(TAG_USER + 3, head) && !FindTagItem(TAG_USER + 4, head));     /* skipped */
    UC(!FindTagItem(TAG_USER + 6, head) && !FindTagItem(TAG_USER + 8, head));
    UC(!FindTagItem(TAG_IGNORE, head) && !FindTagItem(TAG_MORE, head));
    UC(GetTagData(TAG_USER + 7, 0, head) == 70 && GetTagData(TAG_USER + 9, 1234, head) == 1234);
    UC(GetTagData(TAG_USER, 5, nullptr) == 5);
    walk = dead_end;                            /* TAG_MORE to nowhere: the end */
    UC(NextTagItem(&walk) == &dead_end[0] && !NextTagItem(&walk) && !walk);
    UC(TagInArray(TAG_USER, array) && TagInArray(7, array));
    UC(!TagInArray(11, array) && !TagInArray(8, array) && !TagInArray(7, nullptr));
    return 0;
}

static unsigned uc_hooks(unsigned *cases)
{
    struct Hook h = { uc_hook_entry, nullptr, (void *)(uintptr_t)1000 };
    uint64_t msg = 7;

    UC(CallHookPkt(&h, (void *)(uintptr_t)2, &msg) == 1013);
    h.h_Entry = nullptr;
    UC(CallHookPkt(&h, nullptr, &msg) == 0 && CallHookPkt(nullptr, nullptr, &msg) == 0);
    return 0;
}

static unsigned uc_strings(unsigned *cases)
{
    char buf[16];

    for (int c = 0; c < 256; c++) {
        bool lower = c >= 'a' && c <= 'z', upper = c >= 'A' && c <= 'Z';

        UC(ToUpper(c) == (lower ? c - 32 : c) && ToLower(c) == (upper ? c + 32 : c));
    }
    UC(Stricmp("Hello", "hELLO") == 0 && Stricmp("", "") == 0);
    UC(Stricmp("abc", "ABD") < 0 && Stricmp("abd", "ABC") > 0);
    UC(Stricmp("ab", "abc") < 0 && Stricmp("abc", "AB") > 0);
    UC(Stricmp("a\xe9", "A\xc9") > 0);         /* not Latin-1's case: UTF-8's bytes */
    UC(Stricmp("\x80", "a") > 0);               /* bytes as unsigned */
    UC(Strnicmp("PREFIXed", "prefixES", 6) == 0 && Strnicmp("abc", "abd", 3) < 0);
    UC(Strnicmp("x", "y", 0) == 0 && Strnicmp("same", "SAME", 100) == 0);

    UC(SNPrintf(buf, sizeof buf, "%d-%s", 42, "ok") == 5 && uc_same(buf, "42-ok"));
    UC(SNPrintf(buf, sizeof buf, "%s", "exactly fifteen") == 15 &&
       uc_same(buf, "exactly fifteen"));
    UC(SNPrintf(buf, sizeof buf, "%s!", "exactly fifteen") == 16 &&
       uc_same(buf, "exactly fifteen"));       /* cut, and ended */
    UC(SNPrintf(buf, 4, "%lu", 1234567UL) == 7 && uc_same(buf, "123"));
    buf[0] = 'z';
    UC(SNPrintf(buf, 1, "abc") == 3 && buf[0] == '\0');
    buf[0] = 'z';
    UC(SNPrintf(buf, 0, "abc") == 3 && buf[0] == 'z');     /* size 0 writes nothing */
    UC(SNPrintf(nullptr, 0, "%08x", 1u) == 8);
    UC(SNPrintf(buf, sizeof buf, "") == 0 && buf[0] == '\0');
    UC(SNPrintf(buf, sizeof buf, "[%S]", STR("str")) == 5 && uc_same(buf, "[str]"));
    return 0;
}

static unsigned uc_strs(unsigned *cases)
{
    struct Str s = STR("  key = value\r\n"), head, tail, t;
    uint64_t v;

    UC(StrFromC("four").st_Len == 4 && StrFromC(nullptr).st_Len == 0 && StrFromC("").st_Len == 0);
    UC(StrSame(StrSlice(STR("abcdef"), 1, 4), STR("bcd")));
    UC(StrSame(StrSlice(STR("abc"), 1, 99), STR("bc")) && StrSlice(STR("abc"), 5, 9).st_Len == 0);
    UC(StrSlice(STR("abc"), 2, 1).st_Len == 0);
    t = StrTrim(s);
    UC(StrSame(t, STR("key = value")));
    UC(StrTrim(STR(" \t\r\n ")).st_Len == 0 && StrTrim(STR("")).st_Len == 0);
    UC(StrSplit(t, '=', &head, &tail) && StrSame(StrTrim(head), STR("key")) &&
       StrSame(StrTrim(tail), STR("value")));
    UC(StrSplit(STR("a=b=c"), '=', &head, &tail) && StrSame(head, STR("a")) &&
       StrSame(tail, STR("b=c")));
    UC(StrSplit(STR("=x"), '=', &head, &tail) && head.st_Len == 0 && StrSame(tail, STR("x")));
    UC(StrSplit(STR("x="), '=', &head, &tail) && StrSame(head, STR("x")) && tail.st_Len == 0);
    UC(!StrSplit(STR("none"), '=', &head, &tail) && StrSame(head, STR("none")) && !tail.st_Len);
    UC(StrSame(STR(""), STR("")) && !StrSame(STR("a"), STR("ab")) && !StrSame(STR("a"), STR("b")));
    UC(StrSameNoCase(STR("FAT32"), STR("fat32")) && !StrSameNoCase(STR("FAT32"), STR("fat16")));
    UC(!StrSameNoCase(STR("\xc3\xa9"), STR("\xc3\x89")));      /* é and É: bytes */
    UC(StrHasPrefix(STR("dos.0"), STR("dos")) && StrHasPrefix(STR("x"), STR("")));
    UC(!StrHasPrefix(STR("do"), STR("dos")) && !StrHasPrefix(STR("dis"), STR("dos")));
    /* The slice is a view: no NUL needed after it. */
    UC(StrSame((struct Str){ "12345", 3 }, STR("123")) && StrToU64((struct Str){ "12345", 3 }, 10, &v) &&
       v == 123);

    UC(StrToU64(STR("0"), 10, &v) && v == 0);
    UC(StrToU64(STR("18446744073709551615"), 10, &v) && v == UINT64_MAX);
    v = 77;
    UC(!StrToU64(STR("18446744073709551616"), 10, &v) && v == 77);     /* overflow */
    UC(!StrToU64(STR("99999999999999999999999"), 10, &v) && v == 77);
    UC(StrToU64(STR("ffffffffffffffff"), 16, &v) && v == UINT64_MAX);
    UC(!StrToU64(STR("10000000000000000"), 16, &v));
    UC(StrToU64(STR("DeadBeef"), 16, &v) && v == 0xdeadbeef);
    UC(StrToU64(STR("z"), 36, &v) && v == 35 && !StrToU64(STR("z"), 35, &v));
    UC(StrToU64(STR("0x1F"), 0, &v) && v == 31 && StrToU64(STR("0B101"), 0, &v) && v == 5);
    UC(StrToU64(STR("0o17"), 0, &v) && v == 15 && StrToU64(STR("017"), 0, &v) && v == 17);
    UC(StrToU64(STR("0"), 0, &v) && v == 0);
    UC(!StrToU64(STR("0x"), 0, &v) && !StrToU64(STR(""), 10, &v) && !StrToU64(STR("12a"), 10, &v));
    UC(!StrToU64(STR("-1"), 10, &v) && !StrToU64(STR(" 1"), 10, &v) && !StrToU64(STR("1 "), 10, &v));
    UC(!StrToU64(STR("1"), 1, &v) && !StrToU64(STR("1"), 37, &v));
    return 0;
}

static unsigned uc_spans(unsigned *cases)
{
    static const uint8_t bytes[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a };
    struct Span s = { bytes, sizeof bytes }, sub;
    uint8_t b;
    uint16_t h;
    uint32_t w;
    uint64_t d;

    UC(SpanReadU8(s, 9, &b) && b == 0x0a);
    UC(SpanReadU16LE(s, 0, &h) && h == 0x0201 && SpanReadU16BE(s, 0, &h) && h == 0x0102);
    UC(SpanReadU32LE(s, 1, &w) && w == 0x05040302 && SpanReadU32BE(s, 1, &w) && w == 0x02030405);
    UC(SpanReadU64LE(s, 2, &d) && d == 0x0a09080706050403 && SpanReadU64BE(s, 2, &d) &&
       d == 0x030405060708090a);
    /* At the end, and one past: refused, and *out as it was. */
    UC(SpanReadU16BE(s, 8, &h) && h == 0x090a);
    h = 0x5555, w = 0x55555555, d = 0x5555555555555555, b = 0x55;
    UC(!SpanReadU8(s, 10, &b) && !SpanReadU16LE(s, 9, &h) && !SpanReadU32BE(s, 7, &w) &&
       !SpanReadU64LE(s, 3, &d));
    /* Offsets that would wrap a 64-bit add past the check. */
    UC(!SpanReadU32LE(s, UINT64_MAX - 1, &w) && !SpanReadU64BE(s, UINT64_MAX - 6, &d) &&
       !SpanReadU8(s, UINT64_MAX, &b));
    UC(b == 0x55 && h == 0x5555 && w == 0x55555555 && d == 0x5555555555555555);

    UC(SpanSlice(s, 4, 6, &sub) && sub.sp_Data == bytes + 4 && sub.sp_Len == 6);
    UC(SpanReadU16BE(sub, 0, &h) && h == 0x0506 && !SpanReadU16BE(sub, 5, &h));
    UC(SpanSlice(s, 10, 0, &sub) && sub.sp_Len == 0 && !SpanReadU8(sub, 0, &b));
    sub = (struct Span){ nullptr, 99 };
    UC(!SpanSlice(s, 4, 7, &sub) && !SpanSlice(s, 11, 0, &sub) && sub.sp_Len == 99);
    UC(!SpanSlice(s, 1, UINT64_MAX, &sub) && !SpanSlice(s, UINT64_MAX, 2, &sub));
    UC(!SpanReadU8((struct Span){ nullptr, 0 }, 0, &b));
    return 0;
}

/* 0 if all held; else the line of the check that didn't. */
static unsigned util_check(unsigned *cases)
{
    unsigned line;

    if ((line = uc_tags(cases)) || (line = uc_hooks(cases)) || (line = uc_strings(cases)) ||
        (line = uc_strs(cases)) || (line = uc_spans(cases)))
        return line;
    return 0;
}

#undef UC

#endif /* CONFORMANCE_UTILCHECK_H */
