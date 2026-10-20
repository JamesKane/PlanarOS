/*
 * arch/rv64/lazy.S -- saving and loading FP and vector registers
 *
 * The kernel is built without F, D or V; only these routines use them,
 * with sstatus.FS or .VS switched on by the caller (lazy.c).
 */

    .section .text
    .option push
    .option arch, +d, +v

/* void riscv_fp_save(uint64_t *area): f0-f31, then fcsr. */
    .globl  riscv_fp_save
riscv_fp_save:
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    fsd     f\n, \n*8(a0)
    .endr
    frcsr   t0
    sd      t0, 32*8(a0)
    ret

/* void riscv_fp_load(const uint64_t *area) */
    .globl  riscv_fp_load
riscv_fp_load:
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    fld     f\n, \n*8(a0)
    .endr
    ld      t0, 32*8(a0)
    fscsr   t0
    ret

/*
 * void riscv_vec_save(uint64_t *area): vstart, vl, vtype and vcsr, then
 * v0-v31 as whole registers (vlenb bytes each). Whole-register stores
 * ignore vl and vtype, so nothing is changed by saving.
 */
    .globl  riscv_vec_save
riscv_vec_save:
    csrr    t0, vstart
    sd      t0, 0*8(a0)
    csrr    t0, vl
    sd      t0, 1*8(a0)
    csrr    t0, vtype
    sd      t0, 2*8(a0)
    csrr    t0, vcsr
    sd      t0, 3*8(a0)
    addi    a0, a0, 4*8
    csrr    t1, vlenb
    slli    t1, t1, 3               /* eight registers at a time */
    vs8r.v  v0, (a0)
    add     a0, a0, t1
    vs8r.v  v8, (a0)
    add     a0, a0, t1
    vs8r.v  v16, (a0)
    add     a0, a0, t1
    vs8r.v  v24, (a0)
    ret

/* void riscv_vec_load(const uint64_t *area) */
    .globl  riscv_vec_load
riscv_vec_load:
    addi    t2, a0, 4*8
    csrr    t1, vlenb
    slli    t1, t1, 3
    vl8re8.v v0, (t2)
    add     t2, t2, t1
    vl8re8.v v8, (t2)
    add     t2, t2, t1
    vl8re8.v v16, (t2)
    add     t2, t2, t1
    vl8re8.v v24, (t2)
    /* vl and vtype go back together; vstart last, since vector
     * instructions (the loads above) reset it. */
    ld      t0, 1*8(a0)
    ld      t1, 2*8(a0)
    vsetvl  x0, t0, t1
    ld      t0, 3*8(a0)
    csrw    vcsr, t0
    ld      t0, 0*8(a0)
    csrw    vstart, t0
    ret

    .option pop
