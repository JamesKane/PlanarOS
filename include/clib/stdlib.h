/*
 * clib/stdlib.h -- C's memory, numbers, sorting and exit (docs §17.4.5)
 *
 * malloc and its family are exec.library's AllocVec and FreeVec, from
 * the pool every task of the process shares: a block from malloc can be
 * given to FreeVec, and one from AllocVec to free. exit and abort end
 * the process (ExitProcess), its tasks with it; there's no atexit.
 */
#ifndef CLIB_STDLIB_H
#define CLIB_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* Aligned to 16, as max_align_t; malloc(0) is a block of its own. */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);        /* nullptr if n * size overflows */
void *realloc(void *p, size_t size);
void free(void *p);
size_t memalignment(const void *p);         /* C23's: 0 for nullptr */

__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);
__attribute__((noreturn)) void abort(void);   /* logged, then EXIT_FAILURE */

long strtol(const char *restrict s, char **restrict end, int base);
long long strtoll(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end, int base);

/* qsort is a heapsort: no recursion and no allocation, so it doesn't
 * fail, and it isn't stable. */
void qsort(void *base, size_t n, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*compar)(const void *, const void *));

int abs(int j);
long labs(long j);
long long llabs(long long j);

#endif /* CLIB_STDLIB_H */
