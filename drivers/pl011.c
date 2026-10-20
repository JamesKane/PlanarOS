/*
 * drivers/pl011.c -- polled Arm PL011 console
 */
#include "pl011.h"

constexpr uintptr_t UARTDR    = 0x00;     /* data register */
constexpr uintptr_t UARTFR    = 0x18;     /* flag register */
constexpr uint32_t UARTFR_TXFF = 1u << 5; /* transmit FIFO full */

void pl011_putc(const struct pl011 *u, char c)
{
    while (*(volatile uint32_t *)(u->base + UARTFR) & UARTFR_TXFF)
        ;
    *(volatile uint32_t *)(u->base + UARTDR) = (uint8_t)c;
}
