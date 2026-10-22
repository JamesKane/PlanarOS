/*
 * kernel/console.c -- the kernel's early console, from the firmware description
 *
 * The UART drivers here are the polled kind the firmware leaves set up;
 * the kernel never programs baud rates. The serial server takes over
 * later. From the device tree, the console is /chosen's stdout-path;
 * from ACPI, the SPCR's port (kernel/acpi.h).
 */
#include <stdint.h>

#include "arch.h"

#include "acpi.h"
#include "console.h"
#include "devmem.h"
#include "fdt.h"
#include "hw.h"
#include "kprintf.h"
#include "pl011.h"
#include "plat.h"
#include "uart8250.h"

enum console_kind { CONSOLE_PLAT, CONSOLE_8250, CONSOLE_PL011 };

static enum console_kind kind = CONSOLE_PLAT;
static struct uart8250 u8250;
static struct pl011 pl011;
static int32_t console_node = -1;
static uint64_t console_pa;
static const char *console_table;      /* from ACPI: "SPCR" or "DBG2" */

/* Device-tree compatibles of 8250-family UARTs the firmware leaves usable
 * by register writes alone. The K1's UART is an XScale variant; the
 * RK3588's a DesignWare APB UART. */
static const char *const compat_8250[] = {
    "ns16550a", "ns16550", "ns8250", "snps,dw-apb-uart", "intel,xscale-uart",
    nullptr,
};

static bool try_node(int32_t node)
{
    uint64_t base, size;

    if (node < 0 || !fdt_reg(node, 0, &base, &size))
        return false;
    console_pa = base;
    devmem_claim(base, size ? size : 1);
    if (fdt_is_compatible(node, "arm,pl011")) {
        pl011 = (struct pl011){ .base = (uintptr_t)phys_to_virt(base) };
        kind = CONSOLE_PL011;
        console_node = node;
        return true;
    }
    for (const char *const *c = compat_8250; *c; c++) {
        if (fdt_is_compatible(node, *c)) {
            u8250 = (struct uart8250){
                .base = (uintptr_t)phys_to_virt(base),
                .reg_shift = (uint8_t)fdt_getprop_u32(node, "reg-shift", 0),
                .reg_width = (uint8_t)fdt_getprop_u32(node, "reg-io-width", 1),
            };
            uart8250_probe_fifo(&u8250);
            kind = CONSOLE_8250;
            console_node = node;
            return true;
        }
    }
    return false;
}

static bool try_acpi(void)
{
    struct acpi_console c;

    if (!acpi_console(&c))
        return false;
    console_pa = c.base;
    devmem_claim(c.base, 1);
    console_table = c.table;
    if (c.kind == ACPI_UART_PL011) {
        pl011 = (struct pl011){ .base = (uintptr_t)phys_to_virt(c.base) };
        kind = CONSOLE_PL011;
    } else {
        u8250 = (struct uart8250){
            .base = (uintptr_t)phys_to_virt(c.base),
            .reg_shift = c.reg_shift, .reg_width = c.reg_width,
        };
        uart8250_probe_fifo(&u8250);
        kind = CONSOLE_8250;
    }
    return true;
}

bool console_init(void)
{
    uint32_t len;
    const char *path;

    if (hw_source() == HW_ACPI)
        return try_acpi();
    path = fdt_getprop(fdt_path("/chosen"), "stdout-path", &len);
    if (!path)
        path = fdt_getprop(fdt_path("/chosen"), "linux,stdout-path", &len);
    if (path) {
        /* "serial0:115200n8" or "/soc/serial@10000000": the node is the
         * part before any ':' options. */
        uint32_t n = 0;

        while (n < len && path[n] && path[n] != ':')
            n++;
        if (try_node(fdt_path_or_alias(path, n)))
            return true;
    }
    return false;
}

/* Where the console was found: its node, or its ACPI table. */
static const char *console_name(void)
{
    return console_table ? console_table : fdt_node_name(console_node);
}

void console_report(void)
{
    switch (kind) {
    case CONSOLE_8250:
        kprintf("planar: console %s, 8250 at %p, reg-shift %u, width %u\n",
                console_name(), (void *)u8250.base,
                (unsigned)u8250.reg_shift, (unsigned)u8250.reg_width);
        break;
    case CONSOLE_PL011:
        kprintf("planar: console %s, PL011 at %p\n", console_name(), (void *)pl011.base);
        break;
    case CONSOLE_PLAT:
        kprintf("planar: console from the platform (%s)\n", plat_name);
        break;
    }
}

void console_describe(void)
{
    switch (kind) {
    case CONSOLE_8250:
        kprintf("hw: console 8250 %p shift %u width %u\n", (void *)console_pa,
                (unsigned)u8250.reg_shift, (unsigned)u8250.reg_width);
        break;
    case CONSOLE_PL011:
        kprintf("hw: console pl011 %p\n", (void *)console_pa);
        break;
    case CONSOLE_PLAT:
        kprintf("hw: console platform\n");
        break;
    }
}

void console_putc(char c)
{
    switch (kind) {
    case CONSOLE_8250:
        uart8250_putc(&u8250, c);
        break;
    case CONSOLE_PL011:
        pl011_putc(&pl011, c);
        break;
    case CONSOLE_PLAT:
        plat_console_putc(c);
        break;
    }
}

unsigned console_room(void)
{
    switch (kind) {
    case CONSOLE_8250:
        return uart8250_room(&u8250);
    case CONSOLE_PL011:
        return pl011_room(&pl011);
    default:
        return 1;                   /* the platform's waits itself */
    }
}

int console_getc(void)
{
    switch (kind) {
    case CONSOLE_8250:
        return uart8250_getc(&u8250);
    case CONSOLE_PL011:
        return pl011_getc(&pl011);
    default:
        return -1;
    }
}

uint64_t console_base(void)
{
    return console_pa;
}
