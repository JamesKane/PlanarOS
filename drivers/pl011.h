/*
 * drivers/pl011.h -- polled Arm PL011 console
 *
 * QEMU's aarch64 virt machine; the SBSA-style UARTs on ACPI boards are
 * the same registers.
 */
#ifndef DRIVERS_PL011_H
#define DRIVERS_PL011_H

#include <stdint.h>

struct pl011 {
    uintptr_t base;      /* address of UARTDR */
};

void pl011_putc(const struct pl011 *u, char c);
int pl011_getc(const struct pl011 *u);      /* a byte received, or -1 */

#endif /* DRIVERS_PL011_H */
