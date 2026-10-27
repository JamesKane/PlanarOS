/*
 * arch/aarch64/user/crt0.S -- start of a built-in user program
 *
 * The image header the kernel's loader reads (kernel/process.c), then the
 * entry point: the kernel starts a program here with sp at the top of its
 * stack, x0 and x1 its two arguments and x2 exec.library's interface
 * (IExec). It relocates itself, makes the
 * first task's thread_local block, then main's return value ends the
 * process. _task_start is where CreateTask starts every other task
 * (lib/exec/task.c), with x0 its argument and x1 its function.
 */

/* The block at the top of the stack, and TPIDR_EL0 at it (lib/rt/tls.c):
 * the variables start 64 bytes in, after variant I's 16-byte TCB
 * aligned to the block's 64 (user.ld), whose first word points at the
 * process data. */
    .macro  TLS_BLOCK
    mov     x0, #64
    bl      _tls_size
    mov     x9, sp
    sub     x9, x9, x0
    and     x9, x9, #~63
    mov     sp, x9
    mov     x0, x9
    mov     x1, #64
    bl      _tls_init
    msr     tpidr_el0, x0
    .endm

    .section .text.head, "ax"
    .globl  _start
_start:
    b       1f
    .ascii  "PLNR"                  /* magic */
    .quad   _etext - _start         /* bytes of code and constants (RX) */
    .quad   _end - _start           /* whole image, .bss included */
    .quad   __planar_unwind - _start    /* the unwind table (docs §19.4) */
    .quad   __planar_unwind_end - _start
    .quad   __build_id - _start         /* the build-id note */
    .quad   __build_id_end - _start
1:  mov     x19, x0
    mov     x20, x1
    mov     x21, x2
    mov     x0, #1027               /* R_AARCH64_RELATIVE */
    bl      _relocate               /* lib/rt/reloc.c */
    adrp    x9, IExec               /* lib/rt/tls.c */
    str     x21, [x9, :lo12:IExec]
    TLS_BLOCK
    mov     x0, x19
    mov     x1, x20
    bl      main
    b       ExitProcess

    .text
    .globl  _task_start
_task_start:
    mov     x19, x0
    mov     x20, x1
    TLS_BLOCK
    mov     x0, x19
    blr     x20
    mov     x0, #0
    b       ExitTask
