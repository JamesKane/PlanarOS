/*
 * arch/aarch64/user/cache.c -- keeping DMA memory in step with the caches
 * (planar/drv.h)
 *
 * Planar's Arm machines so far are coherent: QEMU, and the CIX, whose
 * devices snoop (ACPI's _CCA). So these only order the writes before
 * them; a machine that isn't will need dc cvac and civac here.
 */
#include <stdint.h>

void drv_dma_clean(const void *, uint64_t)
{
    __asm__ volatile("dsb sy" : : : "memory");
}

void drv_dma_flush(const void *, uint64_t)
{
    __asm__ volatile("dsb sy" : : : "memory");
}
