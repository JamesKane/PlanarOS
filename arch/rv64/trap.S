/*
 * arch/rv64/trap.S -- S-mode trap entry and return, and faulting probes
 *
 * sscratch is 0 while the hart runs kernel code. While a task runs in
 * user mode (V1 step 4) it holds the top of that task's kernel stack: a
 * trap from user mode swaps it into sp, a trap from the kernel stays on
 * the current stack. Either way the frame goes on the kernel stack.
 * struct trap_frame in trap.c matches this layout: x0-x31, sepc, sstatus.
 * From omega's trap.S.
 */

#define FRAME_SIZE  (34 * 8)
#define SSTATUS_SPP (1 << 8)

    .section .text
    .balign 4
    .globl  trap_entry
trap_entry:
    csrrw   sp, sscratch, sp        /* from user: sp = kernel stack, sscratch = user sp */
    bnez    sp, 1f
    csrrw   sp, sscratch, sp        /* from the kernel: put it all back */
1:  addi    sp, sp, -FRAME_SIZE
    sd      x1, 1*8(sp)
    .irp    n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd      x\n, \n*8(sp)
    .endr
    csrr    t0, sscratch            /* the user sp, or 0 from the kernel */
    bnez    t0, 2f
    addi    t0, sp, FRAME_SIZE      /* the kernel sp at the time of the trap */
2:  sd      t0, 2*8(sp)
    csrw    sscratch, zero
    csrr    t0, sepc
    sd      t0, 32*8(sp)
    csrr    t0, sstatus
    sd      t0, 33*8(sp)

    mv      a0, sp
    call    trap_handler

/* Pop the frame at sp and return to wherever it says. */
    .globl  trap_return
trap_return:
    ld      t0, 33*8(sp)
    csrw    sstatus, t0
    andi    t0, t0, SSTATUS_SPP
    bnez    t0, 3f
    addi    t0, sp, FRAME_SIZE      /* back to user: next trap lands here */
    csrw    sscratch, t0
3:  ld      t0, 32*8(sp)
    csrw    sepc, t0
    ld      x1, 1*8(sp)
    .irp    n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\n, \n*8(sp)
    .endr
    ld      sp, 2*8(sp)
    sret

/*
 * FIXUP insn, resume: if 'insn' traps, the handler puts scause in a0 and
 * resumes at 'resume'.
 */
    .macro  FIXUP insn, resume
    .pushsection .fixup_table, "a"
    .balign 8
    .dword  \insn, \resume
    .popsection
    .endm

/* uint64_t probe_ld(uint64_t addr, uint64_t *val) */
    .globl  probe_ld
probe_ld:
.Lld:
    ld      t0, 0(a0)
    sd      t0, 0(a1)
    li      a0, 0
.Lld_done:
    ret
    FIXUP   .Lld, .Lld_done

/* uint64_t probe_stimecmp(uint64_t *val) */
    .globl  probe_stimecmp
probe_stimecmp:
.Lstimecmp:
    csrr    t0, 0x14d               /* stimecmp */
    sd      t0, 0(a0)
    li      a0, 0
.Lstimecmp_done:
    ret
    FIXUP   .Lstimecmp, .Lstimecmp_done
