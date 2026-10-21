/*
 * kernel/bootinfo.c -- what a loader hands the kernel besides a device tree
 */
#include "arch.h"
#include "bootinfo.h"
#include "plat.h"

static const struct boot_info *info;

uint64_t bootinfo_init(uint64_t firmware_data)
{
    const struct boot_info *b = phys_to_virt(firmware_data);

    if (!firmware_data || b->magic != BOOT_INFO_MAGIC)
        return firmware_data;
    info = b;
    return b->dtb;
}

const struct boot_info *bootinfo(void)
{
    return info;
}

const struct boot_fb *boot_fb(void)
{
    static struct boot_fb board;
    static int known;               /* 0 not looked, 1 none, 2 the board's */

    if (info && info->fb.base)
        return &info->fb;
    if (!known)
        known = plat_framebuffer(&board) ? 2 : 1;
    return known == 2 ? &board : nullptr;
}
