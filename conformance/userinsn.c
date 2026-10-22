/*
 * conformance/userinsn.c -- instructions user mode may use (review §10.8)
 *
 * The ISA's own (arch/<isa>/user/insn.c): on AArch64 wfe, a CTR_EL0
 * read and dc zva, which killed the process while SCTLR_EL1's nTWE and
 * DZE were clear. Exits 0, 2 if one is prohibited, 3 if one went wrong.
 */
#include "insn.h"

int main(void)
{
    return UserInsns();
}
