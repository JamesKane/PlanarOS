/*
 * kernel/kstring.h -- memory and string functions
 *
 * The four standard ones keep their names because the compiler calls
 * them. The kernel's own carry a k prefix so the hosted build never
 * collides with the host's C library.
 */
#ifndef KERNEL_KSTRING_H
#define KERNEL_KSTRING_H

#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t kstrlen(const char *s);
bool kstreq(const char *a, const char *b);

#endif /* KERNEL_KSTRING_H */
