/*
 * arch/aarch64/its.h -- the GICv3 ITS and LPIs, for gicv3.c (its.c)
 */
#ifndef ARCH_AARCH64_ITS_H
#define ARCH_AARCH64_ITS_H

#include <stdint.h>

/* Find the ITS (the device tree, or the MADT) and set up its tables and
 * the LPIs' properties; false if there's none, or it can't be used. */
bool its_init(void);

/* The calling CPU's redistributor, at 'rd' (mapped) and 'rd_pa': its
 * LPIs on, and its collection mapped. */
void its_init_cpu(uintptr_t rd, uint64_t rd_pa);

/* 'n' LPIs for device 'dev', its events 0 to n - 1, and where it writes
 * them; false if there's no room, or no ITS. */
bool its_msi_alloc(uint32_t dev, uint32_t n, uint32_t *first, uint64_t *doorbell);

bool its_is_lpi(uint32_t intid);
uint64_t its_pages(void);          /* made for devices' MSIs, kept for the boot */
uint64_t its_address(void);         /* 0 if there's none in use */
void its_set(uint32_t intid, unsigned level);

#endif /* ARCH_AARCH64_ITS_H */
