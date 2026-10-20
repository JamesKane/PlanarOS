/*
 * arch/rv64/trap.S -- S-mode trap entry and return, and faulting probes
 *
 * sscratch is 0 while the hart runs kernel code. While a task runs in
 * user mode it holds that task's "slot": the 16 bytes at the top of the
 * frame area on its kernel stack, whose first word is the struct cpu the
 * task last left the kernel on. A trap from user mode swaps the slot into
 * sp, puts the frame below it, and reloads tp from it: user code owns tp.
 * A trap from the kernel stays on the current stack.
 *
 * A return to the kernel never reloads tp from the frame: a task
 * preempted in an interrupt may resume on another CPU, and tp must stay
 * that CPU's. A return to user mode writes this CPU into the slot first.
 *
 * struct trap_frame in trap.c matches this layout: x0-x31, sepc, sstatus.
 */

#define FRAME_SIZE  (34 * 8)
#define SSTATUS_SPP (1 << 8)
#define SSTATUS_SIE (1 << 1)

    .section .text
    .balign 4
    .globl  trap_entry
trap_entry:
    csrrw   sp, sscratch, sp        /* from user: sp = slot, sscratch = user sp */
    bnez    sp, from_user
    csrrw   sp, sscratch, sp        /* from the kernel: put it all back */
    addi    sp, sp, -FRAME_SIZE
    sd      x1, 1*8(sp)
    .irp    n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd      x\n, \n*8(sp)
    .endr
    addi    t0, sp, FRAME_SIZE      /* the kernel sp at the time of the trap */
    sd      t0, 2*8(sp)
    j       saved
from_user:
    addi    sp, sp, -FRAME_SIZE
    sd      x1, 1*8(sp)
    .irp    n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd      x\n, \n*8(sp)
    .endr
    csrr    t0, sscratch
    sd      t0, 2*8(sp)             /* the user sp */
    ld      tp, FRAME_SIZE(sp)      /* this CPU, from the slot */
saved:
    csrw    sscratch, zero
    csrr    t0, sepc
    sd      t0, 32*8(sp)
    csrr    t0, sstatus
    sd      t0, 33*8(sp)

    mv      a0, sp
    call    trap_handler

/*
 * Pop the frame at sp and return to wherever it says. sstatus is written
 * with SIE clear whatever the frame holds: an interrupt taken between
 * here and sret would find sscratch already pointing at the slot, take
 * itself for a trap from user mode and build its frame over this one.
 */
    .globl  trap_return
trap_return:
    ld      t0, 33*8(sp)
    andi    t0, t0, ~SSTATUS_SIE
    csrw    sstatus, t0
    andi    t0, t0, SSTATUS_SPP
    bnez    t0, to_kernel
    addi    t0, sp, FRAME_SIZE      /* the slot */
    sd      tp, 0(t0)               /* where the next trap finds this CPU */
    csrw    sscratch, t0
    ld      t0, 32*8(sp)
    csrw    sepc, t0
    ld      x1, 1*8(sp)
    .irp    n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\n, \n*8(sp)
    .endr
    ld      sp, 2*8(sp)
    sret
to_kernel:
    ld      t0, 32*8(sp)
    csrw    sepc, t0
    ld      x1, 1*8(sp)
    .irp    n, 3,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\n, \n*8(sp)
    .endr
    ld      sp, 2*8(sp)
    sret

/* void trap_resume(struct trap_frame *f): return through a frame built in C. */
    .globl  trap_resume
trap_resume:
    mv      sp, a0
    j       trap_return

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

/* uint64_t probe_vlenb(uint64_t *val); needs sstatus.VS on */
    .globl  probe_vlenb
probe_vlenb:
.Lvlenb:
    csrr    t0, 0xc22               /* vlenb */
    sd      t0, 0(a0)
    li      a0, 0
.Lvlenb_done:
    ret
    FIXUP   .Lvlenb, .Lvlenb_done

/* uint64_t probe_copy_from_user(void *dst, uint64_t src, size_t n): 0, or
 * scause. Called with sstatus.SUM set, so user pages are readable. */
    .globl  probe_copy_from_user
probe_copy_from_user:
    beqz    a2, .Lin_done
.Lin:
    lbu     t0, 0(a1)
    sb      t0, 0(a0)
    addi    a0, a0, 1
    addi    a1, a1, 1
    addi    a2, a2, -1
    bnez    a2, .Lin
.Lin_done:
    li      a0, 0
.Lin_out:
    ret
    FIXUP   .Lin, .Lin_out

/* uint64_t probe_copy_to_user(uint64_t dst, const void *src, size_t n): 0,
 * or scause. Called with sstatus.SUM set. */
    .globl  probe_copy_to_user
probe_copy_to_user:
    beqz    a2, .Lout_done
.Lout_load:
    lbu     t0, 0(a1)
.Lout_store:
    sb      t0, 0(a0)
    addi    a0, a0, 1
    addi    a1, a1, 1
    addi    a2, a2, -1
    bnez    a2, .Lout_load
.Lout_done:
    li      a0, 0
.Lout_ret:
    ret
    FIXUP   .Lout_store, .Lout_ret
