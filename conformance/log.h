/*
 * conformance/log.h -- KLog with numbers, without a C library
 */
#ifndef CONFORMANCE_LOG_H
#define CONFORMANCE_LOG_H

#include <planar/exec.h>

struct logbuf {
    char text[160];
    unsigned len;
};

static inline void log_str(struct logbuf *b, const char *s)
{
    while (*s && b->len < sizeof b->text - 1)
        b->text[b->len++] = *s++;
    b->text[b->len] = '\0';
}

static inline void log_num(struct logbuf *b, uint64_t v)
{
    char digits[24];
    unsigned n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n && b->len < sizeof b->text - 1)
        b->text[b->len++] = digits[--n];
    b->text[b->len] = '\0';
}

#endif /* CONFORMANCE_LOG_H */
