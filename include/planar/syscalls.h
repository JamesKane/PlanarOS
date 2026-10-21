/*
 * planar/syscalls.h -- kernel call numbers and errors (the trap ABI)
 *
 * Shared by the kernel and the user-side stubs. RV64: number in a7,
 * arguments in a0-a5, result in a0. AArch64: number in x8, arguments in
 * x0-x5, result in x0. A negative result is an error.
 */
#ifndef PLANAR_SYSCALLS_H
#define PLANAR_SYSCALLS_H

#define SYS_KLOG            1
#define SYS_EXIT_PROCESS    2
#define SYS_SET_SIGNAL      3
#define SYS_WAIT            4
#define SYS_SIGNAL          5
#define SYS_FIND_TASK       6
#define SYS_CREATE_TASK     7
#define SYS_EXIT_TASK       8
#define SYS_ALLOC_SIGNAL    9
#define SYS_FREE_SIGNAL     10
#define SYS_ALLOC_AREA      11
#define SYS_FREE_AREA       12
#define SYS_SHARE_AREA      13
#define SYS_UNSHARE_AREA    14
#define SYS_PROTECT_AREA    15
#define SYS_QUERY_AREA      16
#define SYS_CREATE_PORT     17
#define SYS_DELETE_PORT     18
#define SYS_OPEN_PORT       19
#define SYS_ACCEPT_CONN     20
#define SYS_SIGNAL_CONN     21
#define SYS_SIGNAL_WAIT_CONN 22
#define SYS_CLOSE_CONN      23
#define SYS_CREATE_PROCESS  24
#define SYS_START_PROCESS   25
#define SYS_KILL_PROCESS    26
#define SYS_QUERY_PROCESS   27
#define SYS_CLOSE_HANDLE    28
#define SYS_CURRENT_TIME    29
#define SYS_ARM_TIMER       30
#define SYS_CREATE_LEASE    31
#define SYS_GRANT_LEASE     32
#define SYS_REVOKE_LEASE    33
#define SYS_QUERY_LEASE     34
#define SYS_MAP_LEASE       35
#define SYS_BIND_IRQ        36
#define SYS_ACK_WAIT_IRQ    37
#define SYS_ALLOC_DMA_AREA  38
#define SYS_MAP_FIRMWARE    39
#define SYS_GRANT_FIRMWARE  40
#define SYS_ALLOC_MSI       41
#define SYS_QUERY_SYSTEM    42
#define SYS_CREATE_PROCESS_IMAGE 43
#define SYS_GRANT_CONSOLE   44
#define SYS_READ_CONSOLE    45
#define SYS_WAIT_ADDR       46
#define SYS_WAKE_ADDR       47
#define SYS_SHARE_LEASE     48
#define SYS_UNSHARE_LEASE   49
#define SYS_LIMIT_LEASE_DMA 50

#define ERR_NOSYS           (-1)
#define ERR_BADADDR         (-2)
#define ERR_BADHANDLE       (-3)
#define ERR_NOMEM           (-4)
#define ERR_NOTFOUND        (-5)
#define ERR_BADARG          (-6)
#define ERR_EXISTS          (-7)
#define ERR_RIGHTS          (-8)        /* rights the Area's flags don't allow */
#define ERR_FULL            (-9)        /* a ring is full */
#define ERR_PORT_DIED       (-10)       /* the other end of a connection went */
#define ERR_LIMIT           (-11)       /* a per-process limit (handles, tasks, connections) */
#define ERR_BUSY            (-12)       /* in use: a lease granted or stopping */
#define ERR_TIMEOUT         (-13)       /* a deadline passed first */

#endif /* PLANAR_SYSCALLS_H */
