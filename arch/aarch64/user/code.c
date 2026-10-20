/*
 * arch/aarch64/user/code.c -- "return 42" as AArch64 machine code (conformance/code.h)
 */
#include <planar/syscalls.h>

#include "code.h"

void code_return42(uint32_t out[2])
{
    out[0] = 0x52800540;        /* mov w0, #42 */
    out[1] = 0xd65f03c0;        /* ret */
}

uint64_t code_call_regs(uint64_t)
{
    register uint64_t bad __asm__("x0");

    /* x19-x28 hold known values across the call; x1-x18 (the call number
     * in x8 included) must come back zero (nothing of the kernel's),
     * x19-x28 unchanged. The result is built in x0, which the call
     * returns in anyway. */
    __asm__ volatile(
        ".irp n, 19,20,21,22,23,24,25,26,27,28\n"
        "mov x\\n, #0x500 + \\n\n"
        ".endr\n"
        ".irp n, 2,3,4,5,6,7,9,10,11,12,13,14,15,16,17,18\n"
        "mov x\\n, #-1\n"
        ".endr\n"
        "mov x8, %[nr]\n mov x0, #0\n mov x1, #0\n"
        "svc #0\n"
        "mov x0, #0\n"
        ".irp n, 19,20,21,22,23,24,25,26,27,28\n"
        "sub x\\n, x\\n, #0x500 + \\n\n"
        "orr x0, x0, x\\n\n"
        ".endr\n"
        ".irp n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18\n"
        "orr x0, x0, x\\n\n"
        ".endr\n"
        : "=r"(bad) : [nr] "i"(SYS_SET_SIGNAL)
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
          "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25",
          "x26", "x27", "x28", "memory");
    return bad;
}
