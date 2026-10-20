/*
 * kernel/kprintf.c -- kernel log over the early console
 *
 * One CPU writes at a time, so lines from different CPUs don't
 * interleave. A CPU that faults while writing (a Guru Meditation from
 * inside kprintf) carries on rather than waiting for itself.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "cpu.h"
#include "kprintf.h"

static atomic_uint writer;          /* the writing CPU's index + 1, or 0 */

/* Take the log for this CPU; false if it already had it. */
static bool log_take(unsigned long *was)
{
    unsigned me = this_cpu()->index + 1, none;

    *was = arch_irq_save();
    if (atomic_load_explicit(&writer, memory_order_relaxed) == me)
        return false;
    for (;;) {
        none = 0;
        if (atomic_compare_exchange_weak_explicit(&writer, &none, me, memory_order_acquire,
                                                  memory_order_relaxed))
            return true;
        arch_cpu_relax();
    }
}

static void log_give(bool took, unsigned long was)
{
    if (took)
        atomic_store_explicit(&writer, 0, memory_order_release);
    arch_irq_restore(was);
}

static void put(char c)
{
    if (c == '\n')
        console_putc('\r');
    console_putc(c);
}

static void puts_unlocked(const char *s)
{
    while (*s)
        put(*s++);
}

void kputs(const char *s)
{
    unsigned long was;
    bool took = log_take(&was);

    puts_unlocked(s);
    log_give(took, was);
}

static void put_unsigned(uint64_t v, unsigned base, int width, char pad)
{
    char buf[24];
    int n = 0;

    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width && n < (int)sizeof buf)
        buf[n++] = pad;
    while (n)
        put(buf[--n]);
}

enum length { LEN_INT, LEN_LONG, LEN_LLONG, LEN_SIZE };

static uint64_t arg_unsigned(va_list *ap, enum length len)
{
    switch (len) {
    case LEN_LONG:  return va_arg(*ap, unsigned long);
    case LEN_LLONG: return va_arg(*ap, unsigned long long);
    case LEN_SIZE:  return va_arg(*ap, size_t);
    default:        return va_arg(*ap, unsigned int);
    }
}

static int64_t arg_signed(va_list *ap, enum length len)
{
    switch (len) {
    case LEN_LONG:  return va_arg(*ap, long);
    case LEN_LLONG: return va_arg(*ap, long long);
    case LEN_SIZE:  return (int64_t)va_arg(*ap, size_t);
    default:        return va_arg(*ap, int);
    }
}

void kprintf(const char *fmt, ...)
{
    unsigned long was;
    bool took = log_take(&was);
    va_list ap;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        char pad = ' ';
        int width = 0;
        enum length len = LEN_INT;

        if (*fmt != '%') {
            put(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l') {
            len = LEN_LONG;
            if (*++fmt == 'l') {
                len = LEN_LLONG;
                fmt++;
            }
        } else if (*fmt == 'z') {
            len = LEN_SIZE;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            puts_unlocked(s ? s : "(null)");
            break;
        }
        case 'c':
            put((char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = arg_signed(&ap, len);
            if (v < 0) {
                put('-');
                put_unsigned((uint64_t)0 - (uint64_t)v, 10, width - 1, pad);
            } else {
                put_unsigned((uint64_t)v, 10, width, pad);
            }
            break;
        }
        case 'u':
            put_unsigned(arg_unsigned(&ap, len), 10, width, pad);
            break;
        case 'x':
            put_unsigned(arg_unsigned(&ap, len), 16, width, pad);
            break;
        case 'p':
            puts_unlocked("0x");
            put_unsigned((uintptr_t)va_arg(ap, void *), 16, 16, '0');
            break;
        case '%':
            put('%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            put('%');
            put(*fmt);
            break;
        }
    }
    va_end(ap);
    log_give(took, was);
}
