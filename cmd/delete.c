/*
 * cmd/delete.c -- Delete NAME: remove a file, or an empty directory
 */
#include <planar/dos.h>
#include <planar/syscalls.h>

int main(uint64_t arg)
{
    struct con c;
    struct dos d;
    char cwd[DOS_PATH], path[DOS_PATH];
    const char *args;
    int64_t e;

    dos_args(arg, cwd, sizeof cwd, &args);
    con_open(&c, 16);
    if (!*args || dos_open(&d, 17) != 0) {
        con_puts(&c, !*args ? "Delete: what?\n" : "Delete: no dos\n");
        con_close(&c);
        return 1;
    }
    dos_path(cwd, args, path, sizeof path);
    if ((e = dos_remove(&d, path)) != 0) {
        con_puts(&c, e == ERR_EXISTS ? "Delete: not empty: " : "Delete: can't delete ");
        con_puts(&c, path);
        con_puts(&c, "\n");
        con_close(&c);
        return 1;
    }
    dos_close(&d);
    con_close(&c);
    return 0;
}
