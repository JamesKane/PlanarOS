/*
 * kernel/bootargs.h -- the kernel's arguments
 *
 * Space-separated words: the EFI stub's command line (its load options),
 * or else /chosen/bootargs (QEMU's -append, U-Boot's bootargs).
 */
#ifndef KERNEL_BOOTARGS_H
#define KERNEL_BOOTARGS_H

/* Read them from the firmware description. */
void bootargs_init(void);

/* Is 'word' one of them? */
bool bootargs_has(const char *word);

#endif /* KERNEL_BOOTARGS_H */
