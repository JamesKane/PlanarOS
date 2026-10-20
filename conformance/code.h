/*
 * conformance/code.h -- a few instructions of machine code (per ISA)
 *
 * arch/<isa>/user/code.c writes a function that returns 42, for testing
 * code written into an Area at run time.
 */
#ifndef CONFORMANCE_CODE_H
#define CONFORMANCE_CODE_H

#include <stdint.h>

/* Write "return 42" at 'out' (8 bytes). */
void code_return42(uint32_t out[2]);

/* Make kernel call 'nr' (SetSignal(0, 0)) with the callee-saved
 * registers set, and say what came back wrong: nonzero if one changed,
 * or (RISC-V, where the kernel saves only what the calling convention
 * keeps) if a scratch register came back with anything in it. */
uint64_t code_call_regs(uint64_t nr);

#endif /* CONFORMANCE_CODE_H */
