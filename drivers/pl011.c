/*
 * drivers/pl011.c -- polled Arm PL011 console
 */
#include "pl011.h"

constexpr uintptr_t UARTDR    = 0x00;     /* data register */
constexpr uintptr_t UARTFR    = 0x18;     /* flag register */
constexpr uint32_t UARTFR_TXFF = 1u << 5; /* transmit FIFO full */
constexpr uint32_t UARTFR_RXFE = 1u << 4; /* receive FIFO empty */

void pl011_putc(const struct pl011 *u, char c)
{
    while (*(volatile uint32_t *)(u->base + UARTFR) & UARTFR_TXFF)
        ;
    *(volatile uint32_t *)(u->base + UARTDR) = (uint8_t)c;
}

unsigned pl011_room(const struct pl011 *u)
{
    return *(volatile uint32_t *)(u->base + UARTFR) & UARTFR_TXFF ? 0 : 1;
}

int pl011_getc(const struct pl011 *u)
{
    if (*(volatile uint32_t *)(u->base + UARTFR) & UARTFR_RXFE)
        return -1;
    return *(volatile uint32_t *)(u->base + UARTDR) & 0xff;
}
