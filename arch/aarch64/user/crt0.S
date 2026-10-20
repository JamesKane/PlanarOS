/*
 * arch/aarch64/user/crt0.S -- start of a built-in user program
 *
 * The image header the kernel's loader reads (kernel/process.c), then the
 * entry point: the kernel starts a program here with sp at the top of its
 * stack and x0, x1 its two arguments; main's return value ends the
 * process.
 */

    .section .text.head, "ax"
    .globl  _start
_start:
    b       1f
    .ascii  "PLNR"                  /* magic */
    .quad   _etext                  /* bytes of code and constants (RX) */
    .quad   _end                    /* whole image, .bss included */
1:  bl      main
    b       ExitProcess
