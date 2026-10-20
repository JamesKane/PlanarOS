/*
 * arch/aarch64/user/code.c -- "return 42" as AArch64 machine code (conformance/code.h)
 */
#include "code.h"

void code_return42(uint32_t out[2])
{
    out[0] = 0x52800540;        /* mov w0, #42 */
    out[1] = 0xd65f03c0;        /* ret */
}
