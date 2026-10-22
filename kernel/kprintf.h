/*
 * kernel/kprintf.h -- kernel log
 *
 * Formats: %s %c %d %i %u %x %p %%, with the length modifiers l, ll and z,
 * a '0' flag and a field width. The compiler checks the arguments.
 */
#ifndef KERNEL_KPRINTF_H
#define KERNEL_KPRINTF_H

void kputs(const char *s);

[[gnu::format(printf, 1, 2)]]
void kprintf(const char *fmt, ...);

/* The ring (kprintf.c): its drainer started, once tasks can sleep; its
 * text all shown, here and now (before a halt or a reboot); the IPI's
 * part (kernel/smp.c). */
void klog_start(void);
void klog_flush(void);
void klog_ipi(void);

/* For a selftest: keep the console from everyone for 'ticks', as a
 * slow UART would. */
void klog_hold(uint64_t ticks);

/* A process's text, ready to show: false if the ring has no room. */
bool klog_user(const char *s, unsigned n);

/* The ring's last 'size' bytes at most, oldest first, and where in all
 * the text ever logged they start. */
uint64_t klog_read(char *dst, uint64_t size, uint64_t *at);

#endif /* KERNEL_KPRINTF_H */
