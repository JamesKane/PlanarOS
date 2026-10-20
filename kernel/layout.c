/*
 * kernel/layout.c -- choosing the user layout at boot
 */
#include "bootargs.h"
#include "layout.h"

static enum layout boot = LAYOUT_IDENTITY;

void layout_init(void)
{
    if (bootargs_has("layout=global"))
        boot = LAYOUT_GLOBAL;
}

enum layout layout_boot(void)
{
    return boot;
}

const char *layout_name(enum layout l)
{
    return l == LAYOUT_GLOBAL ? "global" : "identity";
}
