/*
 * arch/rv64/user/string.S -- memcpy, memmove and memset by RVV 1.0
 *
 * In place of lib/rt/string.c's word loops (weak there). Each step
 * moves as many bytes as vsetvli grants at e8 with m8, eight registers
 * together: 256 on QEMU's 128-bit vectors, 512 on the X60's 256-bit
 * ones. Byte elements need no alignment, and a step's loads are all done
 * before its stores, so memmove needs only the direction chosen. The
 * vector unit is switched lazily (lazy.c), so a program's first call
 * costs the kernel a trap.
 */

    .option arch, +v

    .text
    .globl  memcpy
    .globl  memmove
    .globl  memset

/* a0 dst, a1 src, a2 n; returns dst. */
memmove:
    sub     t1, a0, a1
    bgeu    t1, a2, memcpy          /* below the source, or past its end */
    add     a1, a1, a2              /* from the ends, backwards */
    add     a3, a0, a2
1:  vsetvli t0, a2, e8, m8, ta, ma
    sub     a1, a1, t0
    sub     a3, a3, t0
    vle8.v  v0, (a1)
    vse8.v  v0, (a3)
    sub     a2, a2, t0
    bnez    a2, 1b
    ret

memcpy:
    mv      a3, a0
1:  vsetvli t0, a2, e8, m8, ta, ma
    vle8.v  v0, (a1)
    vse8.v  v0, (a3)
    add     a1, a1, t0
    add     a3, a3, t0
    sub     a2, a2, t0
    bnez    a2, 1b
    ret

/* a0 dst, a1 c, a2 n; returns dst. */
memset:
    mv      a3, a0
    vsetvli t0, zero, e8, m8, ta, ma
    vmv.v.x v0, a1
1:  vsetvli t0, a2, e8, m8, ta, ma
    vse8.v  v0, (a3)
    add     a3, a3, t0
    sub     a2, a2, t0
    bnez    a2, 1b
    ret
