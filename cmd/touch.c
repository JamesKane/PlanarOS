/*
 * cmd/touch.c -- Touch FILE: make an empty file if there's none
 */
#include <planar/dos.h>
#include <planar/syscalls.h>

int main(uint64_t arg)
{
    struct con c;
    struct dos d;
    char cwd[DOS_PATH], path[DOS_PATH];
    const char *args;
    int64_t f;

    dos_args(arg, cwd, sizeof cwd, &args);
    con_open(&c, 16);
    if (!*args || dos_open(&d, 17) != 0) {
        con_puts(&c, !*args ? "Touch: which file?\n" : "Touch: no dos\n");
        con_close(&c);
        return 1;
    }
    dos_path(cwd, args, path, sizeof path);
    if ((f = dos_file(&d, path, DOSF_WRITE | DOSF_CREATE, nullptr)) < 0) {
        con_puts(&c, "Touch: can't make ");
        con_puts(&c, path);
        con_puts(&c, "\n");
        con_close(&c);
        return 1;
    }
    dos_close_file(&d, (uint32_t)f);
    dos_close(&d);
    con_close(&c);
    return 0;
}
