/*
 * plat/hosted/plat.c -- hosted build: the console is stdout
 */
#include <stdio.h>

#include "plat.h"

const char plat_name[] = "hosted";

/* A device tree given on the command line describes some other machine:
 * read it, but never drive its devices. The console stays stdout. */
void plat_early_init(void)
{
}

void plat_console_putc(char c)
{
    if (c != '\r')
        putchar(c);
}

void plat_late_init(void)
{
}

/* The firmware's reset works here (SBI SRST, PSCI). */
void plat_reboot(void)
{
}

bool plat_framebuffer(struct boot_fb *)
{
    return false;
}
