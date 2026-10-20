/*
 * kernel/devmem.c -- the device registers the kernel keeps for itself
 *
 * Claimed while the boot CPU is alone, then only read.
 */
#include "devmem.h"
#include "kprintf.h"
#include "pmm.h"

constexpr unsigned CLAIMS = 32;

static struct { uint64_t base, end; } claims[CLAIMS];
static unsigned nclaims;

void devmem_claim(uint64_t base, uint64_t size)
{
    if (nclaims == CLAIMS) {
        kprintf("planar: too many kernel device ranges\n");
        return;
    }
    claims[nclaims].base = base & ~(PAGE_SIZE - 1);
    claims[nclaims].end = (base + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    nclaims++;
}

bool devmem_kernel(uint64_t base, uint64_t size)
{
    for (unsigned i = 0; i < nclaims; i++)
        if (base < claims[i].end && base + size > claims[i].base)
            return true;
    return false;
}
