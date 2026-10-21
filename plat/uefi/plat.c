/*
 * plat/uefi/plat.c -- any Arm board that boots through UEFI with ACPI
 *
 * One image for all of them (docs/v3-plan.md, decision 2): what differs
 * between boards is quirk data keyed by their ACPI IDs (kernel/acpi.c),
 * not code here. The firmware has set up the console UART.
 */
#include "console.h"
#include "plat.h"
#include "sbsa_wdt.h"

const char plat_name[] = "uefi";

void plat_early_init(void)
{
    console_init();
}

/* The ACPI tables name the console (SPCR, or DBG2 by quirk). */
void plat_console_putc(char c)
{
    (void)c;
}

/* The GTDT's watchdog, armed: a hang resets the board (sbsa_wdt.c). */
void plat_late_init(void)
{
    sbsa_wdt_init();
}

/* PSCI's SYSTEM_RESET does it (arch_reboot). */
void plat_reboot(void)
{
}

bool plat_framebuffer(struct boot_fb *)
{
    return false;
}
