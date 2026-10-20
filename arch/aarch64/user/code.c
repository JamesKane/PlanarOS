/*
 * arch/aarch64/user/code.c -- "return 42" as AArch64 machine code (conformance/code.h)
 */
#include "code.h"

void code_return42(uint32_t out[2])
{
    out[0] = 0x52800540;        /* mov w0, #42 */
    out[1] = 0xd65f03c0;        /* ret */
}

uint64_t code_call_regs(uint64_t nr)
{
    uint64_t bad;

    /* x19-x28 hold known values across the call and come back unchanged. */
    __asm__ volatile(
        ".irp n, 19,20,21,22,23,24,25,26,27,28\n"
        "mov x\\n, #0x500 + \\n\n"
        ".endr\n"
        "mov x8, %[nr]\n mov x0, #0\n mov x1, #0\n"
        "svc #0\n"
        "mov %[bad], #0\n"
        ".irp n, 19,20,21,22,23,24,25,26,27,28\n"
        "sub x\\n, x\\n, #0x500 + \\n\n"
        "orr %[bad], %[bad], x\\n\n"
        ".endr\n"
        : [bad] "=&r"(bad) : [nr] "r"(nr)
        : "x0", "x1", "x8", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27",
          "x28", "memory");
    return bad;
}
