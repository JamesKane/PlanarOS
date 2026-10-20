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

#endif /* KERNEL_KPRINTF_H */
