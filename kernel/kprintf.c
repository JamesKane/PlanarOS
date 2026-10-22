/*
 * kernel/kprintf.c -- the kernel log: a ring the console drains
 *
 * kprintf formats into a buffer on the caller's stack and copies the
 * text into the ring, so a log line costs a copy (review M12). Once
 * klog_start() has run, a kernel task drains the ring, writing only what
 * the UART takes without waiting: interrupts are off for microseconds,
 * not the 19 ms a line holds a polled UART at 115200 baud. Before that,
 * when kernel text finds the ring full, and at a halt or a reboot
 * (klog_flush), the caller drains it itself as the polled console did,
 * so no kernel line is lost. A process's line (klog_user) that finds it
 * full waits for room. One CPU writes to the console at a time, in the
 * ring's order; a CPU that faults while writing carries on.
 *
 * The ring keeps what it showed, for klog_read: init's meditation gives
 * hwdb's last lines from it.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "cpu.h"
#include "kprintf.h"
#include "ktime.h"
#include "sched.h"
#include "spinlock.h"

constexpr uint64_t RING = 64 * 1024;
constexpr unsigned LINE = 384;              /* one kprintf's text, before it's split */
constexpr uint64_t DRAIN_US = 500;          /* the drainer's nap while the UART is full */

static char ring[RING];
static spinlock_t ring_lock;                /* appenders */
static _Atomic uint64_t head;               /* bytes ever put in, published after the copy */
static _Atomic uint64_t shown;              /* of those, written out: the writer's alone */
static atomic_uint writer;                  /* the writing CPU's index + 1, or 0 */
static struct task *_Atomic drainer;
static atomic_bool wake_pending;

/* Take the console for this CPU; false if it already had it. */
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

/* The same, but only if no other CPU has it: the drainer naps rather
 * than spin with interrupts off behind a writer. */
static bool log_try(bool *took, unsigned long *was)
{
    unsigned me = this_cpu()->index + 1, none = 0;

    *was = arch_irq_save();
    *took = atomic_load_explicit(&writer, memory_order_relaxed) != me;
    if (!*took || atomic_compare_exchange_strong_explicit(&writer, &none, me,
                                                          memory_order_acquire,
                                                          memory_order_relaxed))
        return true;
    arch_irq_restore(*was);
    return false;
}

static void log_give(bool took, unsigned long was)
{
    if (took)
        atomic_store_explicit(&writer, 0, memory_order_release);
    arch_irq_restore(was);
}

/* Write what's waiting: all of it if 'wait', else what the UART takes
 * now. True once nothing is. The console held. */
static bool show(bool wait)
{
    uint64_t end = atomic_load_explicit(&head, memory_order_acquire);
    uint64_t at = atomic_load_explicit(&shown, memory_order_relaxed);

    while (at < end) {
        unsigned room = wait ? 1 : console_room();

        if (!room)
            break;
        for (; room && at < end; room--)
            console_putc(ring[at++ % RING]);
        atomic_store_explicit(&shown, at, memory_order_release);
    }
    return at == end;
}

static void drain_now(void)
{
    unsigned long was;
    bool took = log_take(&was);

    show(true);
    log_give(took, was);
}

/* Copy 'n' bytes in, if they fit beside what's not yet shown. */
static bool append(const char *s, unsigned n)
{
    unsigned long was = spin_lock_irqsave(&ring_lock);
    uint64_t h = atomic_load_explicit(&head, memory_order_relaxed);

    if (h + n - atomic_load_explicit(&shown, memory_order_acquire) > RING) {
        spin_unlock_irqrestore(&ring_lock, was);
        return false;
    }
    for (unsigned i = 0; i < n; i++)
        ring[(h + i) % RING] = s[i];
    atomic_store_explicit(&head, h + n, memory_order_release);
    spin_unlock_irqrestore(&ring_lock, was);
    return true;
}

/* Kernel text: in the ring, then shown by the drainer, or here if there
 * is none yet. Never dropped: a full ring is drained here first. */
static void emit(const char *s, unsigned n)
{
    while (!append(s, n))
        drain_now();
    if (!atomic_load_explicit(&drainer, memory_order_acquire))
        drain_now();
    else if (!atomic_exchange(&wake_pending, true))
        arch_send_ipi(this_cpu());  /* klog_ipi wakes it: no lock is taken here */
}

void klog_ipi(void)
{
    struct task *t = atomic_load_explicit(&drainer, memory_order_acquire);

    if (t && atomic_exchange(&wake_pending, false))
        task_signal(t, SIGF_LOG);
}

static void drain_task(void *)
{
    for (;;) {
        unsigned long was;
        bool took, done;

        task_wait(SIGF_LOG);
        do {
            done = false;
            if (log_try(&took, &was)) {
                done = show(false);
                log_give(took, was);
            }
            if (!done)
                task_sleep_until(time_now() + time_us(DRAIN_US));
        } while (!done);
    }
}

void klog_start(void)
{
    struct task *t;

    if (atomic_load(&drainer) || !time_have_timers())
        return;
    /* On the boot CPU, where the selftests run themselves: anywhere else
     * it can be the busy CPU a test wanted idle (V3.4 placement, once in
     * five CIX boots). At the top priority, it's never kept waiting. */
    if ((t = task_create("klog", TASK_PRI_MAX, 1, drain_task, nullptr))) {
        atomic_store_explicit(&drainer, t, memory_order_release);
        if (atomic_load(&head) != atomic_load(&shown) && !atomic_exchange(&wake_pending, true))
            arch_send_ipi(this_cpu());
    }
}

void klog_flush(void)
{
    drain_now();
}

void klog_hold(uint64_t ticks)
{
    unsigned long was;
    bool took = log_take(&was);

    for (uint64_t t = time_now(); time_now() - t < ticks;)
        arch_cpu_relax();
    log_give(took, was);
}

bool klog_user(const char *s, unsigned n)
{
    if (!append(s, n))
        return false;
    if (!atomic_load_explicit(&drainer, memory_order_acquire))
        drain_now();
    else if (!atomic_exchange(&wake_pending, true))
        arch_send_ipi(this_cpu());
    return true;
}

uint64_t klog_read(char *dst, uint64_t size, uint64_t *at)
{
    unsigned long was = spin_lock_irqsave(&ring_lock);
    uint64_t h = atomic_load_explicit(&head, memory_order_relaxed);
    uint64_t n = size < h ? size : h;

    if (n > RING)
        n = RING;
    for (uint64_t i = 0; i < n; i++)
        dst[i] = ring[(h - n + i) % RING];
    spin_unlock_irqrestore(&ring_lock, was);
    if (at)
        *at = h - n;
    return n;
}

/* One kprintf's text, gathered on its caller's stack. */
struct out {
    char buf[LINE];
    unsigned n;
};

static void put(struct out *o, char c)
{
    if (o->n + 2 > LINE) {
        emit(o->buf, o->n);
        o->n = 0;
    }
    if (c == '\n')
        o->buf[o->n++] = '\r';
    o->buf[o->n++] = c;
}

static void puts_out(struct out *o, const char *s)
{
    while (*s)
        put(o, *s++);
}

void kputs(const char *s)
{
    struct out o = { .n = 0 };

    puts_out(&o, s);
    if (o.n)
        emit(o.buf, o.n);
}

static void put_unsigned(struct out *o, uint64_t v, unsigned base, int width, char pad)
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
        put(o, buf[--n]);
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
    struct out o = { .n = 0 };
    va_list ap;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        char pad = ' ';
        int width = 0;
        enum length len = LEN_INT;

        if (*fmt != '%') {
            put(&o, *fmt);
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
            puts_out(&o, s ? s : "(null)");
            break;
        }
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = arg_signed(&ap, len);
            if (v < 0) {
                put(&o, '-');
                put_unsigned(&o, (uint64_t)0 - (uint64_t)v, 10, width - 1, pad);
            } else {
                put_unsigned(&o, (uint64_t)v, 10, width, pad);
            }
            break;
        }
        case 'u':
            put_unsigned(&o, arg_unsigned(&ap, len), 10, width, pad);
            break;
        case 'x':
            put_unsigned(&o, arg_unsigned(&ap, len), 16, width, pad);
            break;
        case 'p':
            puts_out(&o, "0x");
            put_unsigned(&o, (uintptr_t)va_arg(ap, void *), 16, 16, '0');
            break;
        case '%':
            put(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }
    va_end(ap);
    if (o.n)
        emit(o.buf, o.n);
}
