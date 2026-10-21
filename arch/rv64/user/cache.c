/*
 * arch/rv64/user/cache.c -- keeping DMA memory in step with the caches
 * (planar/drv.h), by Zicbom
 *
 * The K1's devices don't snoop the caches (Linux's k1.dtsi says
 * dma-noncoherent), so a driver cleans what a device will read and
 * flushes what it will write. The kernel lets user mode do it
 * (arch/rv64/arch.c). 64-byte blocks, the K1's and QEMU's.
 */
#include <stddef.h>
#include <stdint.h>

static void each(const void *p, uint64_t n, unsigned op)
{
    uint64_t a = (uint64_t)p & ~63ULL, end = (uint64_t)p + n;

    __asm__ volatile("fence rw, rw" : : : "memory");
    for (; a < end; a += 64) {
        if (op == 1)
            __asm__ volatile(".insn i 0x0f, 2, x0, 1(%0)" : : "r"(a) : "memory");  /* cbo.clean */
        else
            __asm__ volatile(".insn i 0x0f, 2, x0, 2(%0)" : : "r"(a) : "memory");  /* cbo.flush */
    }
    __asm__ volatile("fence rw, rw" : : : "memory");
}

void drv_dma_clean(const void *p, uint64_t n)
{
    each(p, n, 1);
}

void drv_dma_flush(const void *p, uint64_t n)
{
    each(p, n, 2);
}
