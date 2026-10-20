/*
 * kernel/irq.h -- interrupt dispatch
 *
 * Kernel-mode handlers for bring-up. User-mode drivers (docs §4.6) build
 * on this in V1 step 4: the claim stays held while the driver task runs
 * and AckIrq completes it.
 */
#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <stdint.h>

/* Largest source number + 1 on any target: GIC SPIs go to 1019, and a
 * PLIC may have up to 1023 sources. */
constexpr uint32_t IRQ_SOURCES = 1024;

typedef void (*irq_handler_t)(void *data);

/* Route interrupt 'source' to 'fn' at interrupt level 1-7. */
bool irq_attach(uint32_t source, unsigned level, irq_handler_t fn, void *data);

/* Called by the architecture's trap code. */
void irq_external(void);        /* the interrupt controller has a source */
void irq_timer(void);           /* the timer deadline passed (RISC-V) */

/* Interrupts taken so far, from the controller and the timer. */
uint64_t irq_count(void);

#endif /* KERNEL_IRQ_H */
