/*
 * kernel/futex.h -- WaitAddr and WakeAddr (planar/exec.h)
 *
 * A task sleeps on a 64-bit word, found by its physical address, until
 * someone wakes the word. The calls act for the running task.
 */
#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

#include <stdint.h>

int64_t futex_wait_call(uint64_t addr, uint64_t value, uint64_t deadline);
int64_t futex_wake_call(uint64_t addr, uint32_t count);

/* Wake every task waiting on a word in [pa, pa + size): an Area one of
 * whose mappings went, perhaps its writer's, dead now (§12.5). The caller
 * may hold area.c's lock. */
void futex_wake_range(uint64_t pa, uint64_t size);

/* Tasks waiting on a word now, for leak checks. */
unsigned futex_waiters(void);

#endif /* KERNEL_FUTEX_H */
