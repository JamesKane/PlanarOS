/*
 * drivers/plic.h -- RISC-V platform-level interrupt controller
 *
 * Used by any board with a standard PLIC (QEMU, the K1). Registered with
 * the kernel as plic_driver (kernel/intc.h); the device tree gives the
 * base address and which context belongs to each hart's S-mode.
 */
#ifndef DRIVERS_PLIC_H
#define DRIVERS_PLIC_H

#include "intc.h"

extern const struct intc_driver plic_driver;

#endif /* DRIVERS_PLIC_H */
