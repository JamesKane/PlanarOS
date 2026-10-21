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
#include "bootinfo.h"
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

/*
 * U-Boot's framebuffer: its K1 display driver sets up one layer on the HDMI
 * path, read-DMA channel 3 fetching ARGB8888 from RAM, and leaves it
 * scanning out after booti (from omega's plat/ky-x1/plat.c, which found it
 * there; the vendor U-Boot's drivers/video/ky/ky_dpu.c). Without a monitor
 * U-Boot never programs the layer and the registers hold junk, so what's
 * read is believed only if it's a plausible framebuffer in DRAM's first
 * bank. ARGB8888 in memory is B, G, R, X: GOP's format 1.
 */
constexpr uint64_t DPU_HDMI = 0xc0440000;
constexpr uint64_t FB_LAYER = 0x0a80 + 0x100 * 3;
constexpr uint32_t FB_MAX_DIM = 4096;

static uint32_t dpu(uint64_t off)
{
    return *(volatile uint32_t *)phys_to_virt(DPU_HDMI + FB_LAYER + off);
}

bool plat_framebuffer(struct boot_fb *fb)
{
    uint32_t size = dpu(0x3c), stride = dpu(0x38);
    uint64_t base = (uint64_t)dpu(0x24) << 32 | dpu(0x20);
    uint32_t w = size & 0xffff, h = size >> 16;

    if (base < 0x80000 || base >= 0x80000000 || (base & 63) || !w || w > FB_MAX_DIM || !h ||
        h > FB_MAX_DIM || stride < w * 4 || stride > FB_MAX_DIM * 4 || stride % 4 ||
        base + (uint64_t)h * stride > 0x80000000)
        return false;
    *fb = (struct boot_fb){ .base = base, .size = (uint64_t)h * stride, .width = w, .height = h,
                            .stride = stride / 4, .format = 1 };
    return true;
}
