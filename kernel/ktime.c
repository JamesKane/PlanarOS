/*
 * kernel/ktime.c -- the monotonic clock and kernel timers
 */
#include "arch.h"
#include "cpu.h"
#include "irq.h"
#include "ktime.h"

static uint64_t hz;
static atomic_uint_fast64_t interrupts;
static bool have_timers;
static uint32_t timer_irq;

static void timer_source(void *)
{
    time_expired();
}

bool time_init(void)
{
    hz = arch_timebase_hz();
    if (!hz || !arch_timer_name())
        return false;
    timer_irq = arch_timer_irq();
    have_timers = timer_irq == 0 || irq_attach(timer_irq, 7, timer_source, nullptr);
    return have_timers;
}

/* The timer is a per-CPU source (a PPI) where it goes through the
 * interrupt controller, so each CPU enables its own. */
void time_init_cpu(void)
{
    arch_timer_arm(~(uint64_t)0);
    if (have_timers && timer_irq)
        irq_attach(timer_irq, 7, timer_source, nullptr);
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

/* Whole seconds and the rest apart: the rest times 10^9 fits in 64
 * bits for any timebase below 18 GHz. */
uint64_t time_ns(uint64_t ticks)
{
    uint64_t rate = time_hz();

    if (!rate)
        return 0;
    return ticks / rate * 1000000000 + ticks % rate * 1000000000 / rate;
}

uint64_t time_from_ns(uint64_t ns)
{
    uint64_t rate = time_hz(), s = ns / 1000000000;

    if (rate && s > ~(uint64_t)0 / rate - 1)
        return ~(uint64_t)0;
    return s * rate + ns % 1000000000 * rate / 1000000000;
}

/* Arm this CPU's hardware for the head of its list. Lock held. */
static void rearm(struct cpu *c)
{
    arch_timer_arm(c->timers ? c->timers->deadline : ~(uint64_t)0);
}

/* Take 't' off its CPU's list. That CPU's lock held. */
static void unlink(struct ktimer *t)
{
    struct ktimer **p;

    for (p = &t->cpu->timers; *p != t; p = &(*p)->next)
        ;
    *p = t->next;
    t->pending = false;
}

void ktimer_start(struct ktimer *t, uint64_t deadline)
{
    struct ktimer **p;
    struct cpu *c;

    ktimer_stop(t);
    unsigned long was = arch_irq_save();
    c = this_cpu();
    spin_lock(&c->timer_lock);
    t->deadline = deadline;
    t->cpu = c;
    for (p = &c->timers; *p && (*p)->deadline <= deadline; p = &(*p)->next)
        ;
    t->next = *p;
    *p = t;
    t->pending = true;
    rearm(c);
    spin_unlock(&c->timer_lock);
    arch_irq_restore(was);
}

/*
 * A timer stopped from another CPU leaves that CPU's hardware armed for
 * it; the interrupt then finds nothing due and re-arms for what's left.
 * If its handler is running there, wait for it to finish, so the caller
 * may free what it uses as soon as this returns.
 */
void ktimer_stop(struct ktimer *t)
{
    struct cpu *c = t->cpu;

    if (!c)
        return;
    unsigned long was = spin_lock_irqsave(&c->timer_lock);
    if (t->pending && t->cpu == c) {
        unlink(t);
        if (c == this_cpu())
            rearm(c);
    }
    spin_unlock_irqrestore(&c->timer_lock, was);
    if (c != this_cpu())
        while (atomic_load(&c->timer_running) == t)
            arch_cpu_relax();
}

uint64_t time_interrupts(void)
{
    return atomic_load(&interrupts);
}

void time_expired(void)
{
    struct cpu *c = this_cpu();
    unsigned long was = spin_lock_irqsave(&c->timer_lock);
    uint64_t now = time_now();

    atomic_fetch_add(&interrupts, 1);
    while (c->timers && c->timers->deadline <= now) {
        struct ktimer *t = c->timers;

        unlink(t);
        /* The handler may start timers, so it runs unlocked. */
        atomic_store(&c->timer_running, t);
        spin_unlock_irqrestore(&c->timer_lock, was);
        t->fn(t->data);
        atomic_store(&c->timer_running, nullptr);
        was = spin_lock_irqsave(&c->timer_lock);
        now = time_now();
    }
    rearm(c);
    spin_unlock_irqrestore(&c->timer_lock, was);
}
