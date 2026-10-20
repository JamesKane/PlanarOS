/*
 * arch/rv64/user/code.c -- "return 42" as RV64 machine code (conformance/code.h)
 */
#include <planar/syscalls.h>

#include "code.h"

void code_return42(uint32_t out[2])
{
    out[0] = 0x02a00513;        /* li a0, 42 */
    out[1] = 0x00008067;        /* ret */
}

uint64_t code_call_regs(uint64_t)
{
    register uint64_t bad __asm__("a0");

    /* s0-s11 hold known values across the call; t0-t6 and a2-a6 must come
     * back zero (nothing of the kernel's), s0-s11 unchanged. The result
     * is built in a0, which the call returns in anyway. */
    __asm__ volatile(
        ".irp n, 0,1,2,3,4,5,6,7,8,9,10,11\n"
        "li s\\n, 0x500 + \\n\n"
        ".endr\n"
        "li t0, -1\n li t1, -1\n li t2, -1\n li t3, -1\n li t4, -1\n li t5, -1\n li t6, -1\n"
        "li a7, %[nr]\n li a0, 0\n li a1, 0\n li a2, -1\n li a3, -1\n li a4, -1\n li a5, -1\n li a6, -1\n"
        "ecall\n"
        "li a0, 0\n"
        ".irp n, 0,1,2,3,4,5,6,7,8,9,10,11\n"
        "addi s\\n, s\\n, -(0x500 + \\n)\n"
        "or a0, a0, s\\n\n"
        ".endr\n"
        "or a0, a0, t0\n or a0, a0, t1\n or a0, a0, t2\n or a0, a0, t3\n"
        "or a0, a0, t4\n or a0, a0, t5\n or a0, a0, t6\n or a0, a0, a2\n"
        "or a0, a0, a3\n or a0, a0, a4\n or a0, a0, a5\n or a0, a0, a6\n"
        : "=r"(bad) : [nr] "i"(SYS_SET_SIGNAL)
        : "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
          "t0", "t1", "t2", "t3", "t4", "t5", "t6", "a1", "a2", "a3", "a4", "a5", "a6",
          "a7", "memory");
    return bad;
}
