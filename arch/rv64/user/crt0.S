/*
 * arch/rv64/user/crt0.S -- start of a built-in user program
 *
 * The image header the kernel's loader reads (kernel/process.c), then the
 * entry point: the kernel starts a program here with sp at the top of its
 * stack and a0, a1 its two arguments. It relocates itself, then main's return value ends the
 * process.
 */

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
    mv      a0, s0
    mv      a1, s1
    call    main
    tail    ExitProcess
