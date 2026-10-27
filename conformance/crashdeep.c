/*
 * conformance/crashdeep.c -- a fault three calls deep, through a shared
 * library (docs §19.11 step 1)
 *
 * main calls outer, which has exec.library format a string (RawDoFmt);
 * for its first byte the library calls the program's put, which calls
 * deepest, which writes to 0x20. The crash record must show each frame,
 * innermost first: deepest, put, exec.library's (one or more), outer,
 * main. kernel/selftest.c checks where each one is, and on the host
 * planar-symbolize names them.
 */
#include <stdarg.h>

#include <planar/exec.h>

[[gnu::noinline]] static void deepest(volatile char *p, char ch)
{
    *p = ch;                            /* #0 */
}

[[gnu::noinline]] static void put(char ch, void *data)
{
    deepest(data, ch);                  /* #1 */
    *(volatile char *)data = 0;
}

[[gnu::noinline]] static uint64_t outer(void *data, const char *fmt, ...)
{
    va_list ap;
    uint64_t n;

    va_start(ap, fmt);
    n = IExec->RawDoFmt(fmt, ap, put, data);    /* exec.library's frames, then this */
    va_end(ap);
    return n + 1;
}

int main(void)
{
    return (int)outer((void *)0x20, "%d", 42);  /* main */
}
