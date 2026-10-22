/*
 * arch/rv64/user/crt0.S -- start of a built-in user program
 *
 * The image header the kernel's loader reads (kernel/process.c), then the
 * entry point: the kernel starts a program here with sp at the top of its
 * stack and a0, a1 its two arguments. It relocates itself, makes the
 * first task's thread_local block, then main's return value ends the
 * process. _task_start is where CreateTask starts every other task
 * (lib/exec/task.c), with a0 its argument and a1 its function.
 */

/* The block at the top of the stack, and tp at it (lib/rt/tls.c): the
 * variables start at tp itself. */
    .macro  TLS_BLOCK
    li      a0, 0
    call    _tls_size
    sub     sp, sp, a0
    andi    sp, sp, -64
    mv      a0, sp
    li      a1, 0
    call    _tls_init
    mv      tp, a0
    .endm

    .section .text.head, "ax"
    .option norvc                   /* a 4-byte jump: the header is at fixed offsets */
    .globl  _start
_start:
    j       1f
    .ascii  "PLNR"                  /* magic */
    .dword  _etext - _start         /* bytes of code and constants (RX) */
    .dword  _end - _start           /* whole image, .bss included */
1:  mv      s0, a0
    mv      s1, a1
    li      a0, 3                   /* R_RISCV_RELATIVE */
    call    _relocate               /* lib/rt/reloc.c */
    TLS_BLOCK
    mv      a0, s0
    mv      a1, s1
    call    main
    tail    ExitProcess

    .text
    .globl  _task_start
_task_start:
    mv      s0, a0
    mv      s1, a1
    TLS_BLOCK
    mv      a0, s0
    jalr    s1
    li      a0, 0
    tail    ExitTask
