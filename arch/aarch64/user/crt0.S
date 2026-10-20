/*
 * arch/aarch64/user/crt0.S -- start of a built-in user program
 *
 * The image header the kernel's loader reads (kernel/process.c), then the
 * entry point: the kernel starts a program here with sp at the top of its
 * stack and x0, x1 its two arguments. It relocates itself, then main's return value ends the
 * process.
 */

    .section .text.head, "ax"
    .globl  _start
_start:
    b       1f
    .ascii  "PLNR"                  /* magic */
    .quad   _etext - _start         /* bytes of code and constants (RX) */
    .quad   _end - _start           /* whole image, .bss included */
1:  mov     x19, x0
    mov     x20, x1
    mov     x0, #1027               /* R_AARCH64_RELATIVE */
    bl      _relocate               /* lib/rt/reloc.c */
    mov     x0, x19
    mov     x1, x20
    bl      main
    b       ExitProcess
