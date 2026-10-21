/*
 * servers/acpid/clib.c -- the C library functions ACPICA calls, on Planar
 *
 * acplanar.h declares them; lib/rt/string.c has the memory functions. On
 * the host they're the host's. The character tests are the C locale's.
 */
#include <stddef.h>

int isdigit(int c);
int isspace(int c);
int isxdigit(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *restrict dst, const char *restrict src);
char *strcat(char *restrict dst, const char *restrict src);

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isprint(int c)
{
    return c >= 0x20 && c < 0x7f;
}

int toupper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

int tolower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    return 0;
}

char *strcpy(char *restrict dst, const char *restrict src)
{
    char *d = dst;

    while ((*d++ = *src++))
        ;
    return dst;
}

char *strcat(char *restrict dst, const char *restrict src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}
