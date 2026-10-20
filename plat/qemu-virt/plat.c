/*
 * plat/qemu-virt/plat.c -- QEMU's 'virt' machine, riscv64 and aarch64
 *
 * One plat/ for both architectures: everything V0 needs comes from the
 * device tree QEMU generates, so nothing here depends on which one it is.
 */
#include "console.h"
#include "plat.h"

const char plat_name[] = "qemu-virt";

void plat_early_init(void)
{
    /* The firmware (OpenSBI, or QEMU itself) has already set up the UART. */
    console_init();
}

/* The device tree always names the console; there is no fallback. */
void plat_console_putc(char c)
{
    (void)c;
}

void plat_late_init(void)
{
}

/* The firmware's reset works here (SBI SRST, PSCI). */
void plat_reboot(void)
{
}
