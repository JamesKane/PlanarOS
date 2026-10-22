/*
 * arch/aarch64/user/insn.c -- instructions user mode may use (conformance/insn.h)
 *
 * wfe (after sevl, so it returns at once), a CTR_EL0 read, and dc zva,
 * which must zero its block. Each killed the process while SCTLR_EL1's
 * nTWE and DZE were clear.
 */
#include <planar/exec.h>

#include "insn.h"

int UserInsns(void)
{
    alignas(2048) uint8_t block[2048];  /* not static: every program links this file */
    uint64_t dczid, ctr;
    unsigned size;

    __asm__ volatile("sevl\n\twfe");
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    (void)ctr;
    if (dczid & 16)
        return 2;                   /* DZP: dc zva prohibited */
    size = 4u << (dczid & 15);
    if (size > sizeof block)
        return 2;
    SetMem(block, 0xa5, size);
    __asm__ volatile("dc zva, %0" : : "r"(block) : "memory");
    for (unsigned i = 0; i < size; i++)
        if (block[i])
            return 3;
    return 0;
}
