/*
 * clib/ctype.h -- C's character tests, the C locale's (docs §17.4.5)
 *
 * ASCII only: a byte above 0x7f is in no class and isn't changed, as
 * utility.library's ToUpper leaves it. Each is inline, so none is a
 * function a program can take the address of.
 */
#ifndef CLIB_CTYPE_H
#define CLIB_CTYPE_H

static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c)  { return isupper(c) || islower(c); }
static inline int isalnum(int c)  { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isblank(int c)  { return c == ' ' || c == '\t'; }
static inline int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7f; }
static inline int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
static inline int isgraph(int c)  { return c > 0x20 && c < 0x7f; }
static inline int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
static inline int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }

#endif /* CLIB_CTYPE_H */
