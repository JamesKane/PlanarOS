/*
 * kernel/spinlock.h -- spinlocks
 *
 * Test-and-test-and-set over C23 atomics. Taken with interrupts held off
 * on this CPU, so an interrupt handler that takes the same lock can't
 * deadlock against the code it interrupted.
 */
#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <stdatomic.h>

#include "arch.h"

typedef struct {
    atomic_bool locked;
} spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
    while (atomic_exchange_explicit(&l->locked, true, memory_order_acquire))
        while (atomic_load_explicit(&l->locked, memory_order_relaxed))
            arch_cpu_relax();
}

static inline void spin_unlock(spinlock_t *l)
{
    atomic_store_explicit(&l->locked, false, memory_order_release);
}

[[nodiscard]] static inline unsigned long spin_lock_irqsave(spinlock_t *l)
{
    unsigned long was = arch_irq_save();

    spin_lock(l);
    return was;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, unsigned long was)
{
    spin_unlock(l);
    arch_irq_restore(was);
}

#endif /* KERNEL_SPINLOCK_H */
