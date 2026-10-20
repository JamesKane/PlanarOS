/*
 * drivers/sbsa_wdt.c -- the Arm SBSA generic watchdog, from the GTDT
 *
 * Two frames: control (WCS, WOR) and refresh (WRR). It counts the system
 * counter; when WOR's period runs out it raises WS0 (an interrupt, which
 * the kernel leaves masked), and after a second period WS1, which resets
 * the board. Any write to WOR is an explicit refresh, as is one to WRR.
 *
 * The kernel keeps it armed and refreshes it from a timer, so a kernel
 * that hangs or halts with interrupts off is reset (docs/v3-plan.md,
 * decision 1): a board in the test loop needs no one at its power
 * switch. The refresh is a WOR write, not WRR: the CIX's refresh frame
 * doesn't refresh it (FreeBSD and Linux both say so).
 */
#include "acpi.h"
#include "arch.h"
#include "kprintf.h"
#include "ktime.h"
#include "sbsa_wdt.h"

constexpr uint32_t WCS    = 0x000;      /* control frame */
constexpr uint32_t WOR    = 0x008;      /* 32 bits; 48 in architecture version 1 */
constexpr uint32_t W_IIDR = 0xfcc;
constexpr uint32_t WCS_EN = 1u << 0;
constexpr uint32_t WCS_WS0 = 1u << 1;

constexpr uint64_t TIMEOUT_US = 10000000;   /* to WS0; WS1 resets one period later */

static uintptr_t ctrl;
static uint64_t period;                 /* WOR, in counter ticks */
static struct ktimer refresh;

static volatile uint32_t *reg(uint32_t off)
{
    return (volatile uint32_t *)(ctrl + off);
}

static void write_wor(uint64_t v)
{
    *reg(WOR) = (uint32_t)v;
    if (((*reg(W_IIDR) >> 16) & 0xf) >= 1)
        *reg(WOR + 4) = (uint32_t)(v >> 32);
}

static void refresh_fn(void *)
{
    write_wor(period);
    ktimer_start(&refresh, time_now() + time_us(TIMEOUT_US / 4));
}

bool sbsa_wdt_init(void)
{
    struct acpi_watchdog w;
    uint64_t max;
    uint32_t was;

    if (!acpi_watchdog(0, &w) || !w.control)
        return false;
    ctrl = (uintptr_t)phys_to_virt(w.control);
    was = *reg(WCS);
    max = ((*reg(W_IIDR) >> 16) & 0xf) >= 1 ? (1ULL << 48) - 1 : 0xffffffffULL;
    period = time_us(TIMEOUT_US);
    if (period > max)
        period = max;

    /* Off, a fresh period, then on: WS0 and WS1 clear on the refresh. */
    *reg(WCS) = 0;
    write_wor(period);
    *reg(WCS) = WCS_EN;
    refresh = (struct ktimer){ .fn = refresh_fn };
    ktimer_start(&refresh, time_now() + time_us(TIMEOUT_US / 4));
    kprintf("planar: watchdog at %p: was %s; armed, %llu ms to the interrupt, reset after twice that\n",
            (void *)w.control, was & WCS_EN ? (was & WCS_WS0 ? "running, past WS0" : "running")
                                             : "stopped",
            (unsigned long long)(period * 1000 / time_hz()));
    return true;
}
