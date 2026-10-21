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
    SYSCALL CreateProcess, SYS_CREATE_PROCESS
    SYSCALL StartProcess, SYS_START_PROCESS
    SYSCALL KillProcess, SYS_KILL_PROCESS
    SYSCALL QueryProcess, SYS_QUERY_PROCESS
    SYSCALL CloseHandle, SYS_CLOSE_HANDLE
    SYSCALL CurrentTime, SYS_CURRENT_TIME
    SYSCALL ArmTimer, SYS_ARM_TIMER
    SYSCALL CreateLease, SYS_CREATE_LEASE
    SYSCALL GrantLease, SYS_GRANT_LEASE
    SYSCALL RevokeLease, SYS_REVOKE_LEASE
    SYSCALL QueryLease, SYS_QUERY_LEASE
    SYSCALL MapLease, SYS_MAP_LEASE
    SYSCALL BindIrq, SYS_BIND_IRQ
    SYSCALL AckWaitIrq, SYS_ACK_WAIT_IRQ
    SYSCALL AllocDmaArea, SYS_ALLOC_DMA_AREA
    SYSCALL MapFirmware, SYS_MAP_FIRMWARE
    SYSCALL GrantFirmware, SYS_GRANT_FIRMWARE
    SYSCALL AllocMsi, SYS_ALLOC_MSI
    SYSCALL QuerySystem, SYS_QUERY_SYSTEM
    SYSCALL CreateProcessImage, SYS_CREATE_PROCESS_IMAGE
    SYSCALL GrantConsole, SYS_GRANT_CONSOLE
    SYSCALL ReadConsole, SYS_READ_CONSOLE
    SYSCALL WaitAddr, SYS_WAIT_ADDR
    SYSCALL WakeAddr, SYS_WAKE_ADDR
    SYSCALL ShareLease, SYS_SHARE_LEASE
    SYSCALL UnshareLease, SYS_UNSHARE_LEASE
    SYSCALL LimitLeaseDma, SYS_LIMIT_LEASE_DMA
    SYSCALL SetLeaseCall, SYS_SET_LEASE_CALL
    SYSCALL CallLease, SYS_CALL_LEASE
    SYSCALL ReleaseArea, SYS_RELEASE_AREA
