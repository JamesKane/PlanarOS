/*
 * cmd/echo.c -- Echo: its arguments, to the console
 */
#include <planar/dos.h>

int main(uint64_t arg)
{
    struct con c;
    char cwd[DOS_PATH];
    const char *args;

    dos_args(arg, cwd, sizeof cwd, &args);
    con_open(&c, 16);
    con_puts(&c, args);
    con_puts(&c, "\n");
    con_close(&c);
    return 0;
}
