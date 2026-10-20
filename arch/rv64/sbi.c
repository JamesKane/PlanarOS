/*
 * arch/rv64/sbi.c -- calls into the M-mode firmware (RISC-V SBI)
 */
#include "sbi.h"

struct sbiret sbi_call(uint32_t ext, uint32_t fid, uint64_t a0, uint64_t a1, uint64_t a2)
{
    register uint64_t r0 __asm__("a0") = a0;
    register uint64_t r1 __asm__("a1") = a1;
    register uint64_t r2 __asm__("a2") = a2;
    register uint64_t r6 __asm__("a6") = fid;
    register uint64_t r7 __asm__("a7") = ext;

    __asm__ volatile("ecall"
                     : "+r"(r0), "+r"(r1)
                     : "r"(r2), "r"(r6), "r"(r7)
                     : "memory");
    return (struct sbiret){ .error = (int64_t)r0, .value = r1 };
}

bool sbi_has(uint32_t ext)
{
    struct sbiret r = sbi_call(SBI_EXT_BASE, SBI_BASE_PROBE_EXTENSION, ext, 0, 0);

    return r.error == 0 && r.value != 0;
}
