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

#endif /* CONFORMANCE_CODE_H */
