/*
 * arch/aarch64/vectors.S -- EL1 exception vectors, and faulting probes
 *
 * Sixteen entries, 0x80 bytes apart: {current EL with SP0, current EL with
 * SPx, lower EL AArch64, lower EL AArch32} x {sync, IRQ, FIQ, SError}.
 * The kernel runs on SP_EL1, so its own exceptions come in on "current EL
 * with SPx". Until user mode exists (V1 step 4) the lower-EL entries are
 * as unexpected as SP0's and FIQ.
 *
 * The frame (struct trap_frame in trap.c): x0-x30, sp, elr, spsr.
 */

#define FRAME_SIZE  (34 * 8)

    .macro  SAVE_FRAME
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #16 * 0]
    stp     x2, x3, [sp, #16 * 1]
    stp     x4, x5, [sp, #16 * 2]
    stp     x6, x7, [sp, #16 * 3]
    stp     x8, x9, [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    add     x9, sp, #FRAME_SIZE     /* the sp at the time of the exception */
    stp     x30, x9, [sp, #16 * 15]
    mrs     x10, elr_el1
    mrs     x11, spsr_el1
    stp     x10, x11, [sp, #16 * 16]
    .endm

    .macro  VENTRY handler, kind
    .balign 0x80
    SAVE_FRAME
    mov     x0, sp
    mov     x1, #\kind
    bl      \handler
    b       trap_return
    .endm

    .section .text
    .balign 0x800
    .globl  vectors
vectors:
    VENTRY  trap_unexpected, 0      /* EL1t sync */
    VENTRY  trap_unexpected, 1      /* EL1t IRQ */
    VENTRY  trap_unexpected, 2      /* EL1t FIQ */
    VENTRY  trap_unexpected, 3      /* EL1t SError */
    VENTRY  trap_sync, 4            /* EL1h sync */
    VENTRY  trap_irq, 5             /* EL1h IRQ */
    VENTRY  trap_unexpected, 6      /* EL1h FIQ */
    VENTRY  trap_unexpected, 7      /* EL1h SError */
    VENTRY  trap_unexpected, 8      /* EL0 sync */
    VENTRY  trap_unexpected, 9      /* EL0 IRQ */
    VENTRY  trap_unexpected, 10     /* EL0 FIQ */
    VENTRY  trap_unexpected, 11     /* EL0 SError */
    VENTRY  trap_unexpected, 12     /* EL0 AArch32 sync */
    VENTRY  trap_unexpected, 13     /* EL0 AArch32 IRQ */
    VENTRY  trap_unexpected, 14     /* EL0 AArch32 FIQ */
    VENTRY  trap_unexpected, 15     /* EL0 AArch32 SError */

/* Pop the frame at sp and return to wherever it says. */
    .globl  trap_return
trap_return:
    ldp     x10, x11, [sp, #16 * 16]
    msr     elr_el1, x10
    msr     spsr_el1, x11
    ldp     x0, x1, [sp, #16 * 0]
    ldp     x2, x3, [sp, #16 * 1]
    ldp     x4, x5, [sp, #16 * 2]
    ldp     x6, x7, [sp, #16 * 3]
    ldp     x8, x9, [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30, [sp, #16 * 15]
    add     sp, sp, #FRAME_SIZE
    eret

/*
 * FIXUP insn, resume: if 'insn' faults, the handler puts ESR_EL1 in x0
 * and resumes at 'resume'.
 */
    .macro  FIXUP insn, resume
    .pushsection .fixup_table, "a"
    .balign 8
    .quad   \insn, \resume
    .popsection
    .endm

/* uint64_t probe_ld(uint64_t addr, uint64_t *val) */
    .globl  probe_ld
probe_ld:
.Lld:
    ldr     x9, [x0]
    str     x9, [x1]
    mov     x0, #0
.Lld_done:
    ret
    FIXUP   .Lld, .Lld_done
