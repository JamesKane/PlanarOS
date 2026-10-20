/*
 * plat/ky-x1/plat.c -- Orange Pi RV2 (Ky X1, a SpacemiT K1)
 *
 * Almost everything comes from U-Boot's own device tree: memory, the
 * harts, the PLIC, the timebase, the console. What's here is what that
 * tree doesn't say, or says wrongly (PlanOmega docs/alpha/target.md):
 *
 * - The console: UART0 is a PXA-style 16550 at 0xd401_7000 with 32-bit
 *   registers 4 bytes apart. If the tree's stdout-path names a UART the
 *   kernel doesn't know, this drives UART0 directly.
 * - The watchdogs: U-Boot starts the SoC's and the PMIC's at 60 s. The
 *   boot script stops the PMIC's (no driver for it yet); the kernel takes
 *   the SoC's over (wdt.c), which also gives the board a reset.
 */
#include "arch.h"
#include "console.h"
#include "plat.h"
#include "uart8250.h"
#include "wdt.h"

const char plat_name[] = "ky-x1 (Orange Pi RV2)";

static const struct uart8250 uart0 = {
    .base = 0xd4017000, .reg_shift = 2, .reg_width = 4,
};

static struct uart8250 fallback;

void plat_early_init(void)
{
    /* U-Boot left UART0 set up at 115200. */
    if (!console_init()) {
        fallback = uart0;
        fallback.base = (uintptr_t)phys_to_virt(uart0.base);
    }
}

void plat_console_putc(char c)
{
    if (fallback.base)
        uart8250_putc(&fallback, c);
}

void plat_late_init(void)
{
    k1_wdt_init();
}

void plat_reboot(void)
{
    k1_wdt_reset();
}
