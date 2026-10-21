/*
 * cmd/type.c -- Type FILE: a file's text, to the console
 */
#include <planar/dos.h>
#include <planar/syscalls.h>

static char buf[4096];

int main(uint64_t arg)
{
    struct con c;
    struct dos d;
    struct DosEntry e;
    char cwd[DOS_PATH], path[DOS_PATH];
    const char *args;
    int64_t f, n;
    uint64_t off = 0;

    dos_args(arg, cwd, sizeof cwd, &args);
    con_open(&c, 16);
    if (!*args || dos_open(&d, 17) != 0) {
        con_puts(&c, !*args ? "Type: which file?\n" : "Type: no dos\n");
        con_close(&c);
        return 1;
    }
    dos_path(cwd, args, path, sizeof path);
    if ((f = dos_file(&d, path, 0, &e)) < 0 || e.de_Dir) {
        con_puts(&c, "Type: can't open ");
        con_puts(&c, path);
        con_puts(&c, "\n");
        con_close(&c);
        return 1;
    }
    while ((n = dos_read(&d, (uint32_t)f, off, buf, sizeof buf)) > 0) {
        con_write(&c, buf, (uint32_t)n);
        off += (uint64_t)n;
    }
    dos_close_file(&d, (uint32_t)f);
    dos_close(&d);
    con_close(&c);
    return n < 0;
}
