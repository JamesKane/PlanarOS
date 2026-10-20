/*
 * arch/rv64/user/syscalls.S -- user-side kernel call stubs
 *
 * Arguments are already where the trap ABI wants them (a0-a5), so each
 * stub only loads the call number.
 */
#include <planar/syscalls.h>

    .macro  SYSCALL name, nr
    .section .text.\name, "ax"
    .globl  \name
\name:
    li      a7, \nr
    ecall
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
    SYSCALL AllocArea, SYS_ALLOC_AREA
    SYSCALL FreeArea, SYS_FREE_AREA
    SYSCALL ShareArea, SYS_SHARE_AREA
    SYSCALL UnshareArea, SYS_UNSHARE_AREA
    SYSCALL ProtectArea, SYS_PROTECT_AREA
    SYSCALL QueryArea, SYS_QUERY_AREA
    SYSCALL CreatePort, SYS_CREATE_PORT
    SYSCALL DeletePort, SYS_DELETE_PORT
    SYSCALL OpenPort, SYS_OPEN_PORT
    SYSCALL AcceptConn, SYS_ACCEPT_CONN
    SYSCALL SignalConn, SYS_SIGNAL_CONN
    SYSCALL SignalWaitConn, SYS_SIGNAL_WAIT_CONN
    SYSCALL CloseConn, SYS_CLOSE_CONN
