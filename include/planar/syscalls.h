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

#define ERR_NOSYS           (-1)
#define ERR_BADADDR         (-2)
#define ERR_BADHANDLE       (-3)
#define ERR_NOMEM           (-4)
#define ERR_NOTFOUND        (-5)
#define ERR_BADARG          (-6)

#endif /* PLANAR_SYSCALLS_H */
