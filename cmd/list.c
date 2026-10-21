/*
 * cmd/list.c -- List [DIR]: a directory's entries, their sizes, and how
 * many (the current directory if none is named)
 */
#include <planar/dos.h>
#include <planar/syscalls.h>

int main(uint64_t arg)
{
    struct con c;
    struct dos d;
    struct DosEntry e;
    char cwd[DOS_PATH], path[DOS_PATH];
    const char *args;
    uint32_t cursor = 0, files = 0, dirs = 0;
    uint64_t bytes = 0;
    int64_t f;

    dos_args(arg, cwd, sizeof cwd, &args);
    con_open(&c, 16);
    if (dos_open(&d, 17) != 0) {
        con_puts(&c, "List: no dos\n");
        con_close(&c);
        return 1;
    }
    dos_path(cwd, args, path, sizeof path);
    if ((f = dos_file(&d, path, 0, &e)) < 0 || !e.de_Dir) {
        con_puts(&c, "List: not a directory: ");
        con_puts(&c, path);
        con_puts(&c, "\n");
        con_close(&c);
        return 1;
    }
    con_puts(&c, "Directory ");
    con_puts(&c, path);
    con_puts(&c, "\n");
    while (dos_next(&d, (uint32_t)f, &cursor, &e) == 0) {
        con_puts(&c, "  ");
        con_puts(&c, e.de_Name);
        if (e.de_Dir) {
            con_puts(&c, " (dir)\n");
            dirs++;
        } else {
            con_puts(&c, "  ");
            con_dec(&c, e.de_Size);
            con_puts(&c, "\n");
            files++;
            bytes += e.de_Size;
        }
    }
    con_dec(&c, files);
    con_puts(&c, " files, ");
    con_dec(&c, dirs);
    con_puts(&c, " directories, ");
    con_dec(&c, bytes);
    con_puts(&c, " bytes\n");
    dos_close_file(&d, (uint32_t)f);
    dos_close(&d);
    con_close(&c);
    return 0;
}
