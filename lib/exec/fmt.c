/*
 * lib/exec/fmt.c -- RawDoFmt, Exec's formatter (docs §17.4.2)
 *
 * C's conversions without floating point, each byte through the
 * caller's procedure, so it allocates nothing and writes anywhere.
 * Exec's sizes were 16-bit %d and 32-bit %ld; these are C's: %d is an
 * int, %ld, %lld, %zd, %td and %jd 64 bits, and C23's %wN and %wfN say
 * the width outright. %b is binary (C23), %S a struct Str, which needs
 * no NUL. A conversion it doesn't know is written as it stands.
 */
#include <planar/exec.h>

struct out {
    PutChProc put;
    void *data;
    uint64_t n;
};

struct spec {
    bool left, plus, space, alt, zero, ptr;
    int64_t width;
    int64_t prec;                   /* -1: none given */
    unsigned bits;                  /* the argument's, for integers */
};

static void put(struct out *o, char c)
{
    o->put(c, o->data);
    o->n++;
}

static void pad(struct out *o, char c, int64_t n)
{
    while (n-- > 0)
        put(o, c);
}

static void string(struct out *o, const struct spec *s, const char *p, uint64_t len)
{
    if (s->prec >= 0 && (uint64_t)s->prec < len)
        len = (uint64_t)s->prec;
    if (!s->left)
        pad(o, ' ', s->width - (int64_t)len);
    for (uint64_t i = 0; i < len; i++)
        put(o, p[i]);
    if (s->left)
        pad(o, ' ', s->width - (int64_t)len);
}

static void number(struct out *o, const struct spec *s, uint64_t v, bool neg, unsigned base,
                   bool upper)
{
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[64], prefix[3];
    unsigned n = 0, np = 0;
    int64_t zeros, total;

    if (neg)
        prefix[np++] = '-';
    else if (s->plus)
        prefix[np++] = '+';
    else if (s->space)
        prefix[np++] = ' ';
    if (s->alt && (v || s->ptr) && (base == 16 || base == 2)) {
        prefix[np++] = '0';
        prefix[np++] = base == 16 ? (upper ? 'X' : 'x') : (upper ? 'B' : 'b');
    }
    if (v || s->prec != 0)                      /* C: precision 0 writes no 0 */
        do {
            digits[n++] = set[v % base];
            v /= base;
        } while (v);
    if (s->alt && base == 8 && (!n || digits[n - 1] != '0'))
        digits[n++] = '0';
    zeros = s->prec > (int64_t)n ? s->prec - (int64_t)n : 0;
    if (s->zero && !s->left && s->prec < 0)
        zeros = s->width - (int64_t)(np + n);
    if (zeros < 0)
        zeros = 0;
    total = (int64_t)(np + n) + zeros;
    if (!s->left)
        pad(o, ' ', s->width - total);
    for (unsigned i = 0; i < np; i++)
        put(o, prefix[i]);
    pad(o, '0', zeros);
    while (n)
        put(o, digits[--n]);
    if (s->left)
        pad(o, ' ', s->width - total);
}

/* A decimal count in the format string; '*' takes one from the arguments. */
static int64_t count(const char **f, va_list *ap)
{
    int64_t v = 0;

    if (**f == '*') {
        (*f)++;
        return va_arg(*ap, int);
    }
    while (**f >= '0' && **f <= '9' && v < 1 << 24)
        v = v * 10 + *(*f)++ - '0';
    return v;
}

/* The length modifier: the integer's width in bits. */
static unsigned length(const char **f)
{
    const char *p = *f;
    unsigned bits = 32;

    switch (*p) {
    case 'h':
        bits = p[1] == 'h' ? (p++, 8) : 16;
        p++;
        break;
    case 'l':
        if (p[1] == 'l')
            p++;
        [[fallthrough]];
    case 'z':
    case 't':
    case 'j':
        bits = 64;
        p++;
        break;
    case 'w': {                     /* C23: wN, wfN */
        const char *q = p + 1 + (p[1] == 'f');
        unsigned w = 0;

        while (*q >= '0' && *q <= '9' && w < 1000)
            w = w * 10 + (unsigned)(*q++ - '0');
        if (w == 8 || w == 16 || w == 32 || w == 64) {
            bits = w;
            p = q;
        }
        break;
    }
    }
    *f = p;
    return bits;
}

static uint64_t unsigned_arg(va_list *ap, unsigned bits)
{
    uint64_t v = bits == 64 ? va_arg(*ap, uint64_t) : va_arg(*ap, unsigned);

    return bits == 64 ? v : v & ((1ULL << bits) - 1);
}

static int64_t signed_arg(va_list *ap, unsigned bits)
{
    int64_t v;

    if (bits == 64)
        return va_arg(*ap, int64_t);
    v = va_arg(*ap, int);
    return bits == 32 ? v : bits == 16 ? (int16_t)v : (int8_t)v;
}

static void convert(struct out *o, const char **f, va_list *ap)
{
    const char *start = *f - 1;     /* at the '%' */
    struct spec s = { .prec = -1 };
    char c;

    for (;; (*f)++) {
        switch (**f) {
        case '-': s.left = true; continue;
        case '+': s.plus = true; continue;
        case ' ': s.space = true; continue;
        case '#': s.alt = true; continue;
        case '0': s.zero = true; continue;
        }
        break;
    }
    s.width = count(f, ap);
    if (s.width < 0) {              /* C: a negative '*' is '-' */
        s.left = true;
        s.width = -s.width;
    }
    if (**f == '.') {
        (*f)++;
        s.prec = count(f, ap);
        if (s.prec < 0)
            s.prec = -1;
    }
    s.bits = length(f);
    switch ((c = *(*f)++)) {
    case 'd':
    case 'i': {
        int64_t v = signed_arg(ap, s.bits);

        number(o, &s, v < 0 ? -(uint64_t)v : (uint64_t)v, v < 0, 10, false);
        break;
    }
    case 'u':
        number(o, &s, unsigned_arg(ap, s.bits), false, 10, false);
        break;
    case 'x':
    case 'X':
        number(o, &s, unsigned_arg(ap, s.bits), false, 16, c == 'X');
        break;
    case 'o':
        number(o, &s, unsigned_arg(ap, s.bits), false, 8, false);
        break;
    case 'b':
    case 'B':
        number(o, &s, unsigned_arg(ap, s.bits), false, 2, c == 'B');
        break;
    case 'p':                       /* 0x, even for 0 */
        s.alt = s.ptr = true;
        number(o, &s, (uintptr_t)va_arg(*ap, void *), false, 16, false);
        break;
    case 'c': {
        char ch = (char)va_arg(*ap, int);

        s.prec = -1;
        string(o, &s, &ch, 1);
        break;
    }
    case 's': {
        const char *p = va_arg(*ap, const char *);
        uint64_t len = 0;

        if (!p)
            p = "(null)";
        while (p[len] && (s.prec < 0 || len < (uint64_t)s.prec))
            len++;
        string(o, &s, p, len);
        break;
    }
    case 'S': {
        struct Str str = va_arg(*ap, struct Str);

        string(o, &s, str.st_Data, str.st_Data ? str.st_Len : 0);
        break;
    }
    case '%':
        put(o, '%');
        break;
    default:                        /* unknown, or the string ended: as it stands */
        if (!c)
            (*f)--;
        while (start < *f)
            put(o, *start++);
    }
}

uint64_t RawDoFmt(const char *formatString, va_list dataStream, PutChProc putChProc,
                  void *putChData)
{
    struct out o = { putChProc, putChData, 0 };
    const char *f = formatString;
    va_list ap;

    va_copy(ap, dataStream);
    while (*f)
        if (*f++ != '%')
            put(&o, f[-1]);
        else
            convert(&o, &f, &ap);
    va_end(ap);
    putChProc('\0', putChData);
    return o.n;
}
