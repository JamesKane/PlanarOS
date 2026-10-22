/*
 * drivers/uart8250.h -- polled 8250/16550 console
 *
 * Shared by every board with an 8250-family UART (RV2, RK3588, QEMU
 * riscv64). The kernel fills in the base address and register spacing
 * from the firmware description.
 */
#ifndef DRIVERS_UART8250_H
#define DRIVERS_UART8250_H

#include <stdint.h>

struct uart8250 {
    uintptr_t base;      /* address of register 0 */
    uint8_t reg_shift;   /* register n is at base + (n << reg_shift) */
    uint8_t reg_width;   /* access width in bytes: 1 or 4 */
    uint8_t fifo;        /* bytes it takes once empty (uart8250_probe_fifo) */
};

void uart8250_putc(const struct uart8250 *u, char c);
/* Whether the firmware left its FIFOs on (IIR's top bits): 16 bytes, the
 * 16550A's, else 1. */
void uart8250_probe_fifo(struct uart8250 *u);
/* Bytes it takes now without waiting: its FIFO's worth once empty, else 0. */
unsigned uart8250_room(const struct uart8250 *u);
int uart8250_getc(const struct uart8250 *u);    /* a byte received, or -1 */

#endif /* DRIVERS_UART8250_H */
