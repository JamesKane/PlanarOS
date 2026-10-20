/*
 * kernel/syscall.h -- the kernel call dispatcher
 */
#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <stdint.h>

/* Run kernel call 'nr' with its six argument registers; returns the
 * result, negative for an error (planar/syscalls.h). Called by the
 * architecture's trap code with interrupts on. */
int64_t syscall_dispatch(uint64_t nr, const uint64_t args[6]);

/* Copy a NUL-terminated string of at most 'size' bytes, NUL included,
 * from user memory. False if it faults or doesn't fit. */
bool syscall_copy_string(char *dst, uint64_t src, unsigned size);

/* A signal bit user code may name: one of the task's own, 16-63. */
bool syscall_sigbit(int64_t bit);

/* KLog lines dropped by the rate limit, for the self-tests. */
uint64_t syscall_klog_drops(void);

/* Kernel calls made so far, for the self-tests. */
uint64_t syscall_count(void);

#endif /* KERNEL_SYSCALL_H */
