/*
 * conformance/insn.h -- instructions user mode may use (per ISA)
 *
 * arch/<isa>/user/insn.c. UserInsns runs them: 0 if each did what it
 * should, 2 if the CPU says one is prohibited, 3 if one did the wrong
 * thing. A trap kills the process instead (review §10.8).
 */
#ifndef CONFORMANCE_INSN_H
#define CONFORMANCE_INSN_H

int UserInsns(void);

#endif /* CONFORMANCE_INSN_H */
