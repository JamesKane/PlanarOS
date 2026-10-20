/*
 * drivers/uart8250.c -- polled 8250/16550 console
 */
#include "uart8250.h"

constexpr unsigned UART_THR      = 0;      /* transmit holding register */
constexpr unsigned UART_LSR      = 5;      /* line status register */
constexpr unsigned UART_LSR_THRE = 0x20;   /* transmit holding register empty */

static uint32_t reg_read(const struct uart8250 *u, unsigned reg)
{
    uintptr_t addr = u->base + ((uintptr_t)reg << u->reg_shift);

    if (u->reg_width == 4)
        return *(volatile uint32_t *)addr;
    return *(volatile uint8_t *)addr;
}

static void reg_write(const struct uart8250 *u, unsigned reg, uint32_t v)
{
    uintptr_t addr = u->base + ((uintptr_t)reg << u->reg_shift);

    if (u->reg_width == 4)
        *(volatile uint32_t *)addr = v;
    else
        *(volatile uint8_t *)addr = (uint8_t)v;
}

void uart8250_putc(const struct uart8250 *u, char c)
{
    while (!(reg_read(u, UART_LSR) & UART_LSR_THRE))
        ;
    reg_write(u, UART_THR, (uint8_t)c);
}
