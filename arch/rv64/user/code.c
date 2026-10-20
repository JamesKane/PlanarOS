/*
 * arch/rv64/user/code.c -- "return 42" as RV64 machine code (conformance/code.h)
 */
#include "code.h"

void code_return42(uint32_t out[2])
{
    out[0] = 0x02a00513;        /* li a0, 42 */
    out[1] = 0x00008067;        /* ret */
}
