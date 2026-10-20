/*
 * kernel/bootargs.c -- the kernel's arguments
 */
#include "bootargs.h"
#include "fdt.h"
#include "kstring.h"

static const char *args;
static uint32_t args_len;

void bootargs_init(void)
{
    args = fdt_getprop(fdt_path("/chosen"), "bootargs", &args_len);
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
