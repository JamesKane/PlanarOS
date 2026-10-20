/*
 * plat/ky-x1/wdt.c -- the K1's SoC watchdog
 *
 * Register layout and sequences from SpacemiT's vendor Linux driver
 * (drivers/watchdog/k1x_wdt.c): every register write is preceded by an
 * unlock pair (WFAR, WSAR); the counter is 16 bits at 256 Hz; a match
 * with WMER = 3 resets the SoC; WCR clears the counter. The MPMU's APRR
 * register holds the watchdog's own reset, which must be released for it
 * to act.
 *
 * U-Boot starts it with a 60 s timeout. The kernel takes it over instead
 * of stopping it: a timer services it every 10 s, so a kernel that hangs
 * or halts (interrupts off) is reset a minute later, and a deliberate
 * reboot is a match at the next tick. U-Boot leaves its clock on.
 */
#include "arch.h"
#include "fdt.h"
#include "kprintf.h"
#include "ktime.h"
#include "wdt.h"

constexpr uint64_t WDT_DEFAULT  = 0xd4080000;
constexpr uint64_t MPMU_DEFAULT = 0xd4050000;

constexpr uint32_t WDT_WFAR = 0x00b0;
constexpr uint32_t WDT_WSAR = 0x00b4;
constexpr uint32_t WDT_WMER = 0x00b8;   /* bit 0 counter, bit 1 reset */
constexpr uint32_t WDT_WMR  = 0x00bc;   /* match value, 256 Hz ticks */
constexpr uint32_t WDT_WSR  = 0x00c0;   /* status */
constexpr uint32_t WDT_WCR  = 0x00c8;   /* write 1: counter back to 0 */
constexpr uint32_t WDT_WVR  = 0x00cc;   /* counter */
constexpr uint32_t MPMU_APRR = 0x1020;
constexpr uint32_t MPMU_APRR_WDTR = 1u << 4;

constexpr unsigned TIMEOUT_S = 60;
constexpr uint64_t SERVICE_US = 10000000;

static uintptr_t wdt, aprr;     /* the watchdog, and the MPMU's APRR register */
static struct ktimer service;

static void write_reg(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(wdt + WDT_WFAR) = 0xbaba;
    *(volatile uint32_t *)(wdt + WDT_WSAR) = 0xeb10;
    *(volatile uint32_t *)(wdt + reg) = v;
}

static uint32_t read_reg(uint32_t reg)
{
    return *(volatile uint32_t *)(wdt + reg);
}

static void service_fn(void *)
{
    write_reg(WDT_WCR, 1);
    ktimer_start(&service, time_now() + time_us(SERVICE_US));
}

void k1_wdt_init(void)
{
    int32_t node = fdt_find_compatible(-1, "ky,x1-wdt");
    uint64_t wdt_pa = WDT_DEFAULT, aprr_pa = MPMU_DEFAULT + MPMU_APRR, pa, size;

    if (node < 0)
        node = fdt_find_compatible(-1, "spacemit,k1x-wdt");
    if (node >= 0) {
        fdt_reg(node, 0, &wdt_pa, &size);
        /* The second entry is the MPMU block in the vendor Linux tree
         * (0xd4050000, 0x1024 bytes) but APRR itself in U-Boot's
         * (0xd4051020, 4 bytes): tell them apart by size. */
        if (fdt_reg(node, 1, &pa, &size))
            aprr_pa = size > MPMU_APRR ? pa + MPMU_APRR : pa;
    }
    wdt = (uintptr_t)phys_to_virt(wdt_pa);
    aprr = (uintptr_t)phys_to_virt(aprr_pa);
    kprintf("planar: watchdog at %p (%s), APRR %p: %s, count %u of %u\n", (void *)wdt_pa,
            node >= 0 ? fdt_node_name(node) : "not in the tree", (void *)aprr_pa,
            read_reg(WDT_WMER) & 1 ? "running" : "stopped", read_reg(WDT_WVR),
            read_reg(WDT_WMR));

    /* Our timeout, counter cleared, counter and reset on. */
    write_reg(WDT_WMR, TIMEOUT_S << 8);
    write_reg(WDT_WCR, 1);
    write_reg(WDT_WMER, 3);
    *(volatile uint32_t *)aprr |= MPMU_APRR_WDTR;
    service = (struct ktimer){ .fn = service_fn };
    ktimer_start(&service, time_now() + time_us(SERVICE_US));
}

/* As the vendor driver's restart handler: match at the next tick. */
void k1_wdt_reset(void)
{
    if (!wdt)
        return;
    ktimer_stop(&service);
    write_reg(WDT_WSR, 0);
    write_reg(WDT_WMR, 1);
    write_reg(WDT_WMER, 3);
    write_reg(WDT_WCR, 1);
    *(volatile uint32_t *)aprr |= MPMU_APRR_WDTR;
    for (uint64_t start = time_now(); time_now() - start < time_us(2000000); )
        ;
    kprintf("planar: reboot failed: the watchdog didn't reset the board\n");
}
