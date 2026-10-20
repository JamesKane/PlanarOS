/*
 * arch/rv64/user/crt0.S -- start of a built-in user program
 *
 * The image header the kernel's loader reads (kernel/process.c), then the
 * entry point: the kernel starts a program here with sp at the top of its
 * stack and a0, a1 its two arguments; main's return value ends the
 * process.
 */

    .section .text.head, "ax"
    .option norvc                   /* a 4-byte jump: the header is at fixed offsets */
    .globl  _start
_start:
    j       1f
    .ascii  "PLNR"                  /* magic */
    .dword  _etext                  /* bytes of code and constants (RX) */
    .dword  _end                    /* whole image, .bss included */
1:  call    main
    tail    ExitProcess
