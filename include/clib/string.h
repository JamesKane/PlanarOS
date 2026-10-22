/*
 * clib/string.h -- C's string functions (docs §17.4.5)
 *
 * lib/clib, for a program that asks for it (-Iinclude/clib and
 * libclib.a). The memory functions are lib/rt's, which every program
 * has; the rest are lib/clib/string.c's. Also C11, for the third-party
 * code that's built as C11.
 */
#ifndef CLIB_STRING_H
#define CLIB_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* C23's: memset that the compiler may not drop, for clearing secrets. */
void *memset_explicit(void *dst, int c, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *restrict dst, const char *restrict src);
char *strncpy(char *restrict dst, const char *restrict src, size_t n);
char *strcat(char *restrict dst, const char *restrict src);
char *strncat(char *restrict dst, const char *restrict src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

#endif /* CLIB_STRING_H */
