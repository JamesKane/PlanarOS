/*
 * arch/aarch64/user/cache.c -- keeping DMA memory in step with the caches
 * (planar/drv.h)
 *
 * For a device that doesn't snoop the CPU's caches (the CIX's DPU, _CCA
 * 0): clean is dc cvac over the range, so what the CPU wrote reaches
 * memory before the device reads it; flush is dc civac, so the CPU reads
 * what the device wrote, not a stale line. The kernel lets EL0 do both
 * and read CTR_EL0 (SCTLR_EL1.UCI and UCT, arch/aarch64/entry.S). Where
 * the device snoops they cost a little and change nothing.
 */
#include <stdint.h>

static uint64_t line(void)
{
    uint64_t ctr;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4ull << (ctr >> 16 & 0xf);       /* DminLine: words */
}

void drv_dma_clean(const void *p, uint64_t n)
{
    uint64_t l = line(), a = (uint64_t)p & ~(l - 1), end = (uint64_t)p + n;

    __asm__ volatile("dsb ishst" : : : "memory");
    for (; a < end; a += l)
        __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

void drv_dma_flush(const void *p, uint64_t n)
{
    uint64_t l = line(), a = (uint64_t)p & ~(l - 1), end = (uint64_t)p + n;

    __asm__ volatile("dsb ishst" : : : "memory");
    for (; a < end; a += l)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}
