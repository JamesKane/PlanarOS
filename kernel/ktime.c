/*
 * kernel/ktime.c -- the monotonic clock and kernel timers
 */
#include "arch.h"
#include "irq.h"
#include "ktime.h"
#include "spinlock.h"

static uint64_t hz;
static struct ktimer *pending;          /* sorted by deadline */
static spinlock_t lock;
static uint64_t interrupts;
static bool have_timers;

static void timer_source(void *)
{
    time_expired();
}

bool time_init(void)
{
    uint32_t source;

    hz = arch_timebase_hz();
    if (!hz || !arch_timer_name())
        return false;
    source = arch_timer_irq();
    have_timers = source == 0 || irq_attach(source, 7, timer_source, nullptr);
    return have_timers;
}

bool time_have_timers(void)
{
    return have_timers;
}

uint64_t time_hz(void)
{
    return hz;
}

uint64_t time_now(void)
{
    return arch_read_time();
}

uint64_t time_us(uint64_t us)
{
    return us * hz / 1000000;
}

uint64_t time_to_us(uint64_t ticks)
{
    return hz ? ticks * 1000000 / hz : 0;
}

/* Arm the hardware for the head of the list. Lock held. */
static void rearm(void)
{
    arch_timer_arm(pending ? pending->deadline : ~(uint64_t)0);
}

void ktimer_start(struct ktimer *t, uint64_t deadline)
{
    unsigned long was = spin_lock_irqsave(&lock);
    struct ktimer **p;

    if (t->pending) {
        for (p = &pending; *p != t; p = &(*p)->next)
            ;
        *p = t->next;
    }
    t->deadline = deadline;
    for (p = &pending; *p && (*p)->deadline <= deadline; p = &(*p)->next)
        ;
    t->next = *p;
    *p = t;
    t->pending = true;
    rearm();
    spin_unlock_irqrestore(&lock, was);
}

void ktimer_stop(struct ktimer *t)
{
    unsigned long was = spin_lock_irqsave(&lock);

    if (t->pending) {
        struct ktimer **p;

        for (p = &pending; *p != t; p = &(*p)->next)
            ;
        *p = t->next;
        t->pending = false;
        rearm();
    }
    spin_unlock_irqrestore(&lock, was);
}

uint64_t time_interrupts(void)
{
    return interrupts;
}

void time_expired(void)
{
    unsigned long was = spin_lock_irqsave(&lock);
    uint64_t now = time_now();

    interrupts++;
    while (pending && pending->deadline <= now) {
        struct ktimer *t = pending;

        pending = t->next;
        t->pending = false;
        /* The handler may start timers, so it runs unlocked. */
        spin_unlock_irqrestore(&lock, was);
        t->fn(t->data);
        was = spin_lock_irqsave(&lock);
        now = time_now();
    }
    rearm();
    spin_unlock_irqrestore(&lock, was);
}
