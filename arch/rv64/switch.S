/*
 * arch/rv64/switch.S -- switching kernel tasks
 *
 * struct arch_context in planar/arch_inline.h matches this layout: ra,
 * sp, s0-s11. From omega's switch.S.
 */

    .section .text

/* void arch_switch(struct arch_context *from, struct arch_context *to) */
    .globl  arch_switch
arch_switch:
    sd      ra, 0*8(a0)
    sd      sp, 1*8(a0)
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11
    sd      s\n, (2+\n)*8(a0)
    .endr
    ld      ra, 0*8(a1)
    ld      sp, 1*8(a1)
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11
    ld      s\n, (2+\n)*8(a1)
    .endr
    ret

/* A new task's first switch lands here. */
    .globl  arch_task_start
arch_task_start:
    tail    task_start
