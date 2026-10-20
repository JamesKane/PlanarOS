/*
 * kernel/devmem.h -- the device registers the kernel keeps for itself
 *
 * The kernel drives a few devices before any server runs: the console,
 * the interrupt controller, the watchdog. Each says here where its
 * registers are, and a lease (kernel/lease.c) may never cover them, so
 * no driver can reach what the kernel depends on (docs/v4-plan.md
 * step 1).
 */
#ifndef KERNEL_DEVMEM_H
#define KERNEL_DEVMEM_H

#include <stdint.h>

/* The kernel drives [base, base + size); rounded out to whole pages. */
void devmem_claim(uint64_t base, uint64_t size);

/* Does [base, base + size) touch anything the kernel drives? */
bool devmem_kernel(uint64_t base, uint64_t size);

#endif /* KERNEL_DEVMEM_H */
