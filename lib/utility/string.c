/*
 * lib/utility/string.c -- case, case-blind comparison and SNPrintf
 * (docs §17.4.3)
 *
 * ASCII's case only: bytes above 0x7f are UTF-8's, and stay as they are.
 */
#include <planar/exec.h>
#include <planar/utility.h>

int ToUpper(int c)
{
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

int ToLower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

int Strnicmp(const char *s1, const char *s2, uint64_t n)
{
    for (; n; n--, s1++, s2++) {
        int a = ToLower((unsigned char)*s1), b = ToLower((unsigned char)*s2);

        if (a != b)
            return a - b;
        if (!a)
            break;
    }
    return 0;
}

int Stricmp(const char *s1, const char *s2)
{
    return Strnicmp(s1, s2, UINT64_MAX);
}

struct sink {
    char *at;
    uint64_t room;                  /* bytes left for text, the NUL's not counted */
};

static void put(char c, void *data)
{
    struct sink *s = data;

    if (!c)
        *s->at = '\0';
    else if (s->room) {
        *s->at++ = c;
        s->room--;
    }
}

static void discard(char c, void *data)
{
    (void)c, (void)data;
}

int64_t VSNPrintf(char *buffer, uint64_t bufferSize, const char *fmt, va_list args)
{
    struct sink s = { buffer, bufferSize ? bufferSize - 1 : 0 };

    if (!buffer || !bufferSize)
        return (int64_t)RawDoFmt(fmt, args, discard, nullptr);
    return (int64_t)RawDoFmt(fmt, args, put, &s);
}

int64_t SNPrintf(char *buffer, uint64_t bufferSize, const char *fmt, ...)
{
    va_list ap;
    int64_t n;

    va_start(ap, fmt);
    n = VSNPrintf(buffer, bufferSize, fmt, ap);
    va_end(ap);
    return n;
}
