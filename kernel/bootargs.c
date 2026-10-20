/*
 * kernel/bootargs.c -- the kernel's arguments
 */
#include "bootargs.h"
#include "bootinfo.h"
#include "fdt.h"
#include "kstring.h"

static const char *args;
static uint32_t args_len;

/* The EFI stub's command line if there is one, else the device tree's. */
void bootargs_init(void)
{
    const struct boot_info *bi = bootinfo();

    if (bi && bi->cmdline[0]) {
        args = bi->cmdline;
        args_len = (uint32_t)kstrlen(bi->cmdline);
    } else {
        args = fdt_getprop(fdt_path("/chosen"), "bootargs", &args_len);
    }
}

bool bootargs_has(const char *word)
{
    size_t n = kstrlen(word);

    for (uint32_t i = 0; args && i + n <= args_len; i++)
        if ((i == 0 || args[i - 1] == ' ') && memcmp(args + i, word, n) == 0 &&
            (i + n == args_len || args[i + n] == ' ' || args[i + n] == '\0'))
            return true;
    return false;
}
