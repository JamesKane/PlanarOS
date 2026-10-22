/*
 * lib/utility/str.c -- strings by their length (docs §17.4.3)
 *
 * A Str is never written through and needs no NUL, so every one of
 * these is a view of the one it was given: slicing copies nothing.
 */
#include <stdckdint.h>

#include <planar/utility.h>

struct Str StrFromC(const char *s)
{
    uint64_t n = 0;

    if (!s)
        return (struct Str){ "", 0 };
    while (s[n])
        n++;
    return (struct Str){ s, n };
}

struct Str StrSlice(struct Str s, uint64_t start, uint64_t end)
{
    if (end > s.st_Len)
        end = s.st_Len;
    if (start > end)
        start = end;
    return (struct Str){ s.st_Data + start, end - start };
}

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

struct Str StrTrim(struct Str s)
{
    while (s.st_Len && blank(s.st_Data[0])) {
        s.st_Data++;
        s.st_Len--;
    }
    while (s.st_Len && blank(s.st_Data[s.st_Len - 1]))
        s.st_Len--;
    return s;
}

bool StrSplit(struct Str s, char sep, struct Str *head, struct Str *tail)
{
    for (uint64_t i = 0; i < s.st_Len; i++)
        if (s.st_Data[i] == sep) {
            *head = (struct Str){ s.st_Data, i };
            *tail = (struct Str){ s.st_Data + i + 1, s.st_Len - i - 1 };
            return true;
        }
    *head = s;
    *tail = (struct Str){ s.st_Data + s.st_Len, 0 };
    return false;
}

bool StrSame(struct Str a, struct Str b)
{
    if (a.st_Len != b.st_Len)
        return false;
    for (uint64_t i = 0; i < a.st_Len; i++)
        if (a.st_Data[i] != b.st_Data[i])
            return false;
    return true;
}

bool StrSameNoCase(struct Str a, struct Str b)
{
    if (a.st_Len != b.st_Len)
        return false;
    for (uint64_t i = 0; i < a.st_Len; i++)
        if (ToLower((unsigned char)a.st_Data[i]) != ToLower((unsigned char)b.st_Data[i]))
            return false;
    return true;
}

bool StrHasPrefix(struct Str s, struct Str prefix)
{
    return prefix.st_Len <= s.st_Len && StrSame((struct Str){ s.st_Data, prefix.st_Len }, prefix);
}

static unsigned digit(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'z')
        return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z')
        return (unsigned)(c - 'A' + 10);
    return 36;
}

bool StrToU64(struct Str s, unsigned base, uint64_t *out)
{
    uint64_t v = 0;

    if (base == 0) {
        base = 10;
        if (s.st_Len > 2 && s.st_Data[0] == '0') {
            switch (ToLower((unsigned char)s.st_Data[1])) {
            case 'x': base = 16; break;
            case 'b': base = 2; break;
            case 'o': base = 8; break;
            }
            if (base != 10)
                s = StrSlice(s, 2, s.st_Len);
        }
    }
    if (base < 2 || base > 36 || !s.st_Len)
        return false;
    for (uint64_t i = 0; i < s.st_Len; i++) {
        unsigned d = digit(s.st_Data[i]);

        if (d >= base || ckd_mul(&v, v, base) || ckd_add(&v, v, d))
            return false;
    }
    *out = v;
    return true;
}
