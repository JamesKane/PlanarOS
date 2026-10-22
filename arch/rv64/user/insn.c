/*
 * arch/rv64/user/insn.c -- instructions user mode may use (conformance/insn.h)
 *
 * pause, a hint every hart takes, spelled out for an assembler without
 * Zihintpause.
 */
#include "insn.h"

int UserInsns(void)
{
    __asm__ volatile(".4byte 0x0100000f");
    return 0;
}
