/*
 * arch/aarch64/user/syscalls.S -- user-side kernel call stubs
 *
 * Arguments are already where the trap ABI wants them (x0-x5), so each
 * stub only loads the call number.
 */
#include <planar/syscalls.h>

    .macro  SYSCALL name, nr
    .section .text.\name, "ax"
    .globl  \name
\name:
    mov     x8, #\nr
    svc     #0
    ret
    .endm

    SYSCALL KLog, SYS_KLOG
    SYSCALL ExitProcess, SYS_EXIT_PROCESS
    SYSCALL SetSignal, SYS_SET_SIGNAL
    SYSCALL Wait, SYS_WAIT
    SYSCALL Signal, SYS_SIGNAL
    SYSCALL FindTask, SYS_FIND_TASK
    SYSCALL CreateTask, SYS_CREATE_TASK
    SYSCALL ExitTask, SYS_EXIT_TASK
    SYSCALL AllocSignal, SYS_ALLOC_SIGNAL
    SYSCALL FreeSignal, SYS_FREE_SIGNAL
