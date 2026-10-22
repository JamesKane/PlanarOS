/*
 * arch/aarch64/user/string.S -- memcpy, memmove and memset by NEON
 *
 * In place of lib/rt/string.c's word loops (weak there). 32 bytes a step
 * through q registers, unaligned: user memory is normal memory, cached
 * or not, where Arm allows it. A length of 32 or more takes its last 32
 * bytes (its first 32, backwards) as one more step, overlapping the
 * steps before it, so there's no byte tail. Shorter lengths are two
 * overlapping loads of the largest size that fits, both made before
 * either store, which also makes them safe for memmove.
 */

    .text
    .globl  memcpy
    .globl  memmove
    .globl  memset

/* x0 dst, x1 src, x2 n; returns dst. Uses x3-x7, q0-q3. */
memmove:
    sub     x3, x0, x1
    cmp     x3, x2
    b.hs    memcpy                  /* below the source, or past its end */
    cmp     x2, #32
    b.lo    small
    ldp     q2, q3, [x1]            /* the first 32: stored last */
    add     x4, x1, x2
    add     x5, x0, x2
1:  ldp     q0, q1, [x4, #-32]!
    stp     q0, q1, [x5, #-32]!
    sub     x2, x2, #32
    cmp     x2, #32
    b.hi    1b
    stp     q2, q3, [x0]
    ret

memcpy:
    cmp     x2, #32
    b.lo    small
    add     x4, x1, x2
    add     x5, x0, x2
    ldp     q2, q3, [x4, #-32]      /* the last 32: stored last */
    mov     x3, x0
1:  ldp     q0, q1, [x1], #32
    stp     q0, q1, [x3], #32
    sub     x2, x2, #32
    cmp     x2, #32
    b.hi    1b
    stp     q2, q3, [x5, #-32]
    ret

/* Fewer than 32 bytes. */
small:
    add     x4, x1, x2
    add     x5, x0, x2
    cmp     x2, #16
    b.lo    1f
    ldr     q0, [x1]
    ldr     q1, [x4, #-16]
    str     q0, [x0]
    str     q1, [x5, #-16]
    ret
1:  cmp     x2, #8
    b.lo    2f
    ldr     x6, [x1]
    ldr     x7, [x4, #-8]
    str     x6, [x0]
    str     x7, [x5, #-8]
    ret
2:  cmp     x2, #4
    b.lo    3f
    ldr     w6, [x1]
    ldr     w7, [x4, #-4]
    str     w6, [x0]
    str     w7, [x5, #-4]
    ret
3:  cbz     x2, 4f                  /* 1 to 3: the first, the middle, the last */
    lsr     x3, x2, #1
    ldrb    w6, [x1]
    ldrb    w7, [x1, x3]
    ldrb    w8, [x4, #-1]
    strb    w6, [x0]
    strb    w7, [x0, x3]
    strb    w8, [x5, #-1]
4:  ret

/* x0 dst, w1 c, x2 n; returns dst. */
memset:
    dup     v0.16b, w1
    add     x5, x0, x2
    cmp     x2, #32
    b.lo    1f
    mov     x3, x0
2:  stp     q0, q0, [x3], #32
    sub     x2, x2, #32
    cmp     x2, #32
    b.hi    2b
    stp     q0, q0, [x5, #-32]
    ret
1:  cmp     x2, #16
    b.lo    3f
    str     q0, [x0]
    str     q0, [x5, #-16]
    ret
3:  fmov    x6, d0
    cmp     x2, #8
    b.lo    4f
    str     x6, [x0]
    str     x6, [x5, #-8]
    ret
4:  cmp     x2, #4
    b.lo    5f
    str     w6, [x0]
    str     w6, [x5, #-4]
    ret
5:  cbz     x2, 6f
    lsr     x3, x2, #1
    strb    w6, [x0]
    strb    w6, [x0, x3]
    strb    w6, [x5, #-1]
6:  ret
