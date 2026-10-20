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

/* Kernel calls made so far, for the self-tests. */
uint64_t syscall_count(void);

#endif /* KERNEL_SYSCALL_H */
