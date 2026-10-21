/*
 * cmd/info.c -- Info: the volumes, how much is free on each, and whether
 * it can be written
 */
#include <planar/dos.h>
#include <planar/syscalls.h>

int main(uint64_t)
{
    struct con c;
    struct dos d;
    struct DosEntry e;

    con_open(&c, 16);
    if (dos_open(&d, 17) != 0) {
        con_puts(&c, "Info: no dos\n");
        con_close(&c);
        return 1;
    }
    con_puts(&c, "Volumes:\n");
    for (uint32_t i = 0; dos_volume(&d, i, &e) == 0; i++) {
        con_puts(&c, "  ");
        con_puts(&c, e.de_Name);
        con_puts(&c, ": ");
        con_dec(&c, e.de_Size >> 20);
        con_puts(&c, e.de_Dir ? " MiB free, read/write\n" : " MiB free, read only\n");
    }
    dos_close(&d);
    con_close(&c);
    return 0;
}
