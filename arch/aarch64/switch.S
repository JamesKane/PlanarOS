/*
 * arch/aarch64/switch.S -- switching kernel tasks
 *
 * struct arch_context in planar/arch_inline.h matches this layout:
 * x19-x28, fp (x29), lr (x30), sp, TPIDR_EL0 (user mode's to use for
 * itself, so it goes with the task).
 */

    .section .text

/* void arch_switch(struct arch_context *from, struct arch_context *to) */
    .globl  arch_switch
arch_switch:
    stp     x19, x20, [x0, #16 * 0]
    stp     x21, x22, [x0, #16 * 1]
    stp     x23, x24, [x0, #16 * 2]
    stp     x25, x26, [x0, #16 * 3]
    stp     x27, x28, [x0, #16 * 4]
    stp     x29, x30, [x0, #16 * 5]
    mov     x9, sp
    mrs     x10, tpidr_el0
    stp     x9, x10, [x0, #16 * 6]
    ldp     x19, x20, [x1, #16 * 0]
    ldp     x21, x22, [x1, #16 * 1]
    ldp     x23, x24, [x1, #16 * 2]
    ldp     x25, x26, [x1, #16 * 3]
    ldp     x27, x28, [x1, #16 * 4]
    ldp     x29, x30, [x1, #16 * 5]
    ldp     x9, x10, [x1, #16 * 6]
    mov     sp, x9
    msr     tpidr_el0, x10
    ret

/* A new task's first switch lands here. */
    .globl  arch_task_start
arch_task_start:
    b       task_start
