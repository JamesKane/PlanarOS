/*
 * kernel/sched.c -- tasks and the SMP scheduler
 */
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"

static struct task idle_tasks[MAX_CPUS];
static atomic_uint_fast64_t switches, steals, handoffs;

[[noreturn]] void task_start(void);

static unsigned level(int pri)
{
    return (unsigned)(pri - TASK_PRI_MIN);
}

static int clamp_pri(int pri)
{
    if (pri < TASK_PRI_MIN + 1)
        return TASK_PRI_MIN + 1;        /* below that is idle's */
    return pri > TASK_PRI_MAX ? TASK_PRI_MAX : pri;
}

static uint64_t bit(const struct cpu *c)
{
    return 1ULL << c->index;
}

/* --- Run queues. Each with its CPU's rq_lock held. --- */

static void enqueue(struct cpu *c, struct task *t)
{
    unsigned l = level(t->pri);

    t->rq_next = nullptr;
    if (c->rq_tail[l])
        c->rq_tail[l]->rq_next = t;
    else
        c->rq_head[l] = t;
    c->rq_tail[l] = t;
    c->rq_bits[l / 64] |= 1ULL << (l % 64);
    c->nr_ready++;
    t->on_rq = true;
    t->cpu = c;
}

/* The same, at the front of its priority: next to run there. */
static void enqueue_head(struct cpu *c, struct task *t)
{
    unsigned l = level(t->pri);

    t->rq_next = c->rq_head[l];
    c->rq_head[l] = t;
    if (!c->rq_tail[l])
        c->rq_tail[l] = t;
    c->rq_bits[l / 64] |= 1ULL << (l % 64);
    c->nr_ready++;
    t->on_rq = true;
    t->cpu = c;
}

static void unlink_rq(struct cpu *c, struct task *t)
{
    unsigned l = level(t->pri);
    struct task *prev = nullptr;

    for (struct task *p = c->rq_head[l]; p != t; p = p->rq_next)
        prev = p;
    if (prev)
        prev->rq_next = t->rq_next;
    else
        c->rq_head[l] = t->rq_next;
    if (c->rq_tail[l] == t)
        c->rq_tail[l] = prev;
    if (!c->rq_head[l])
        c->rq_bits[l / 64] &= ~(1ULL << (l % 64));
    c->nr_ready--;
    t->on_rq = false;
}

/* The highest priority with a ready task, or -1. */
static int top_level(const struct cpu *c)
{
    for (int w = 3; w >= 0; w--)
        if (c->rq_bits[w])
            return w * 64 + 63 - __builtin_clzll(c->rq_bits[w]);
    return -1;
}

static struct task *dequeue_top(struct cpu *c)
{
    int l = top_level(c);
    struct task *t;

    if (l < 0)
        return nullptr;
    t = c->rq_head[l];
    unlink_rq(c, t);
    return t;
}

/* --- The quantum: on only while a peer of the running task waits. --- */

static void quantum_fire(void *)
{
    struct cpu *c = this_cpu();

    c->quantum_on = false;
    atomic_store(&c->need_resched, true);
}

/* rq_lock held. Starts or stops this CPU's quantum timer. */
static void quantum_update(struct cpu *c)
{
    bool peers = c->current != c->idle && top_level(c) >= (int)level(c->current->pri);

    if (peers && !c->quantum_on) {
        c->quantum_on = true;
        ktimer_start(&c->quantum, time_now() + time_us(TASK_QUANTUM_US));
    } else if (!peers && c->quantum_on) {
        c->quantum_on = false;
        ktimer_stop(&c->quantum);
    }
}

/*
 * 't' just became ready on 'c' (rq_lock held). Make 'c' reschedule if
 * it outranks what runs there, or check its quantum if it's a peer.
 * Returns whether 'c' must be interrupted to notice.
 */
static bool wake_check(struct cpu *c, struct task *t)
{
    int cur = c->current->pri;

    if (c->current == c->idle || t->pri > cur) {
        atomic_store(&c->need_resched, true);
        return true;
    }
    if (t->pri == cur) {
        atomic_store(&c->quantum_check, true);
        return true;
    }
    return false;
}

/*
 * Make 'c' notice a wakeup: interrupt it, or on this CPU act now if
 * 'now' (the caller holds no lock), or else interrupt itself, so the
 * switch comes once interrupts are back on and the lock is let go.
 * Inside an interrupt, irq_exit() acts on the way out.
 */
static void kick(struct cpu *c, bool now)
{
    struct cpu *self = this_cpu();

    if (c != self) {
        arch_send_ipi(c);
    } else if (self->irq_depth) {
        return;
    } else if (!now) {
        if (atomic_load(&self->need_resched) || atomic_load(&self->quantum_check))
            arch_send_ipi(self);
    } else if (atomic_load(&self->need_resched)) {
        schedule();
    } else if (atomic_exchange(&self->quantum_check, false)) {
        unsigned long was = spin_lock_irqsave(&self->rq_lock);
        quantum_update(self);
        spin_unlock_irqrestore(&self->rq_lock, was);
    }
}

void sched_kick(struct cpu *c)
{
    unsigned long was = arch_irq_save();

    kick(c, was);
    arch_irq_restore(was);
}

/* --- Switching. --- */

/* The least-loaded online CPU in 'affinity', preferring 'prefer'. */
static struct cpu *place(uint64_t affinity, struct cpu *prefer)
{
    struct cpu *best = nullptr;

    if (prefer && (affinity & bit(prefer)) && atomic_load(&prefer->online))
        best = prefer;
    for (unsigned i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        unsigned load, best_load;

        if (!(affinity & bit(c)) || !atomic_load(&c->online))
            continue;
        load = c->nr_ready + (c->current != c->idle);
        best_load = best ? best->nr_ready + (best->current != best->idle) : ~0u;
        if (load < best_load)
            best = c;
    }
    return best;
}

/*
 * After every switch, on the task switched to: the previous task's
 * context is saved, so it may run elsewhere or be freed.
 */
static void finish_switch(struct task *prev)
{
    atomic_store_explicit(&prev->on_cpu, false, memory_order_release);
    if (prev->migrating) {
        struct cpu *to = place(prev->affinity, nullptr);

        prev->migrating = false;
        if (to) {
            spin_lock(&to->rq_lock);
            enqueue(to, prev);
            bool notify = wake_check(to, prev);
            spin_unlock(&to->rq_lock);
            if (notify)
                kick(to, false);
        }
    } else if (prev->state == TASK_DEAD) {
        struct process *p = prev->process;

        arch_context_free(&prev->ctx);
        for (unsigned i = 0; i < TASK_STACK_PAGES; i++)
            pmm_free(prev->stack + i * PAGE_SIZE);
        if (p)
            process_task_finished(p);
    }
}

void schedule(void)
{
    unsigned long was = arch_irq_save();
    struct cpu *c = this_cpu();
    struct task *prev = c->current, *next;

    spin_lock(&c->rq_lock);
    atomic_store(&c->need_resched, false);
    atomic_store(&c->quantum_check, false);
    if (prev->state == TASK_RUNNING && prev != c->idle) {
        prev->state = TASK_READY;
        if (prev->affinity & bit(c))
            enqueue(c, prev);           /* the back of its priority: round robin */
        else
            prev->migrating = true;     /* finish_switch moves it */
    }
    next = dequeue_top(c);
    if (!next)
        next = c->idle;
    next->state = TASK_RUNNING;
    c->current = next;
    quantum_update(c);
    spin_unlock(&c->rq_lock);

    if (next != prev) {
        /* Its context may still be in use where it last ran. */
        while (atomic_load_explicit(&next->on_cpu, memory_order_acquire))
            arch_cpu_relax();
        atomic_store(&next->on_cpu, true);
        next->runs++;
        atomic_fetch_add(&switches, 1);
        c->switched_from = prev;
        /* A kernel task runs in the kernel's domain alone, so a domain
         * is loaded only while one of its tasks runs (process.c relies
         * on that to reclaim). */
        arch_switch_out(&prev->ctx);
        arch_domain_switch(next->domain);
        arch_switch(&prev->ctx, &next->ctx);
        /* Back here, maybe on another CPU, as 'prev'. */
        finish_switch(this_cpu()->switched_from);
    }
    arch_irq_restore(was);
}

/* A new task's first switch lands here (through arch_task_start). */
[[noreturn]] void task_start(void)
{
    struct task *self;

    finish_switch(this_cpu()->switched_from);
    self = this_cpu()->current;
    arch_irq_enable();
    self->entry(self->data);
    task_exit();
}

/* --- Interrupts. --- */

void irq_enter(void)
{
    this_cpu()->irq_depth++;
}

void irq_exit(void)
{
    struct cpu *c = this_cpu();

    if (--c->irq_depth)
        return;
    if (atomic_load(&c->need_resched)) {
        schedule();
    } else if (atomic_exchange(&c->quantum_check, false)) {
        spin_lock(&c->rq_lock);
        quantum_update(c);
        spin_unlock(&c->rq_lock);
    }
}

/* --- Tasks. --- */

void sched_init_cpu(void)
{
    struct cpu *c = this_cpu();
    struct task *idle = &idle_tasks[c->index];

    *idle = (struct task){ .name = "idle", .pri = TASK_PRI_MIN, .state = TASK_RUNNING,
                           .cpu = c, .affinity = bit(c) };
    atomic_store(&idle->on_cpu, true);
    c->idle = c->current = idle;
    c->quantum = (struct ktimer){ .fn = quantum_fire };
    if (!c->capacity)
        c->capacity = 1024;
}

/* Take a ready task another CPU has queued that may run here. */
static bool steal(struct cpu *self)
{
    for (unsigned i = 0; i < ncpus; i++) {
        struct cpu *src = &cpus[i];
        struct cpu *first = src->index < self->index ? src : self;
        struct cpu *second = first == src ? self : src;
        struct task *got = nullptr;

        if (src == self || src->nr_ready == 0)
            continue;
        spin_lock(&first->rq_lock);
        spin_lock(&second->rq_lock);
        for (int l = top_level(src); l >= 0 && !got; l--)
            for (struct task *t = src->rq_head[l]; t && !got; t = t->rq_next)
                if ((t->affinity & bit(self)) && !atomic_load(&t->on_cpu))
                    got = t;
        if (got) {
            unlink_rq(src, got);
            enqueue(self, got);
            atomic_fetch_add(&steals, 1);
        }
        spin_unlock(&second->rq_lock);
        spin_unlock(&first->rq_lock);
        if (got)
            return true;
    }
    return false;
}

[[noreturn]] void sched_idle(void)
{
    struct cpu *c = this_cpu();

    for (;;) {
        arch_irq_disable();
        if (c->nr_ready == 0 && !steal(c) && !atomic_load(&c->need_resched))
            arch_wait_for_interrupt();
        arch_irq_enable();
        if (c->nr_ready)
            schedule();
    }
}

struct task *task_new(const char *name, int pri, uint64_t affinity,
                      void (*entry)(void *), void *data)
{
    uint64_t stack = pmm_alloc_contig(TASK_STACK_PAGES);
    struct task *t;

    if (!stack)
        return nullptr;
    pri = clamp_pri(pri);
    t = phys_to_virt(stack);
    *t = (struct task){ .name = name, .pri = (int8_t)pri, .base_pri = (int8_t)pri,
                        .state = TASK_READY,
                        .affinity = affinity, .entry = entry, .data = data,
                        .stack = stack };
    arch_context_init(&t->ctx, phys_to_virt(stack + TASK_STACK_PAGES * PAGE_SIZE));
    return t;
}

void task_discard(struct task *t)
{
    uint64_t stack = t->stack;

    for (unsigned i = 0; i < TASK_STACK_PAGES; i++)
        pmm_free(stack + i * PAGE_SIZE);
}

bool task_launch(struct task *t)
{
    unsigned long was = arch_irq_save();
    struct cpu *c = place(t->affinity, this_cpu());

    if (!c) {
        arch_irq_restore(was);
        return false;
    }
    spin_lock(&c->rq_lock);
    enqueue(c, t);
    bool notify = wake_check(c, t);
    spin_unlock(&c->rq_lock);
    if (notify)
        kick(c, was);
    arch_irq_restore(was);
    return true;
}

struct task *task_create(const char *name, int pri, uint64_t affinity,
                         void (*entry)(void *), void *data)
{
    struct task *t = task_new(name, pri, affinity, entry, data);

    if (t && !task_launch(t)) {
        task_discard(t);
        return nullptr;
    }
    return t;
}

[[noreturn]] void task_exit(void)
{
    struct cpu *c;

    arch_irq_disable();
    c = this_cpu();
    spin_lock(&c->rq_lock);
    c->current->state = TASK_DEAD;
    spin_unlock(&c->rq_lock);
    schedule();
    for (;;)
        ;                               /* never resumed */
}

struct task *task_self(void)
{
    return this_cpu()->current;
}

void task_set_affinity(uint64_t affinity)
{
    unsigned long was = arch_irq_save();
    struct task *self = this_cpu()->current;

    self->affinity = affinity;
    arch_irq_restore(was);
    if (!(affinity & bit(this_cpu())))
        schedule();                     /* schedule() sends it on */
}

/* Run 't' at 'pri' from now on (t->lock held, interrupts off); returns
 * the CPU to kick, or nullptr. */
static struct cpu *set_pri(struct task *t, int pri)
{
    struct cpu *c = t->cpu;
    bool notify;

    spin_lock(&c->rq_lock);
    if (t->on_rq) {
        unlink_rq(c, t);
        t->pri = (int8_t)pri;
        enqueue(c, t);
    } else {
        t->pri = (int8_t)pri;
    }
    if (t->state == TASK_READY) {
        notify = wake_check(c, t);
    } else if (t->state == TASK_RUNNING) {
        /* Lowered below something ready there: switch. */
        notify = top_level(c) > (int)level(t->pri);
        if (notify)
            atomic_store(&c->need_resched, true);
    } else {
        notify = false;
    }
    spin_unlock(&c->rq_lock);
    return notify ? c : nullptr;
}

void task_set_pri(struct task *t, int pri)
{
    unsigned long was = spin_lock_irqsave(&t->lock);
    struct cpu *c;

    t->base_pri = (int8_t)clamp_pri(pri);
    c = set_pri(t, t->base_pri);
    spin_unlock_irqrestore(&t->lock, was);
    if (c)
        sched_kick(c);
}

/* Give back a loan; returns the CPU to kick, or nullptr. */
static struct cpu *end_loan(struct task *self)
{
    unsigned long was;
    struct cpu *c;

    if (self->pri == self->base_pri)
        return nullptr;
    was = spin_lock_irqsave(&self->lock);
    c = set_pri(self, self->base_pri);
    spin_unlock_irqrestore(&self->lock, was);
    return c;
}

void task_end_loan(void)
{
    struct cpu *c = end_loan(task_self());

    if (c)
        sched_kick(c);
}

/* --- Signals. --- */

uint64_t task_wait(uint64_t mask)
{
    /* Kicked only if it doesn't wait: waiting switches anyway. */
    struct cpu *loan = end_loan(task_self());

    for (;;) {
        unsigned long was = arch_irq_save();
        struct task *self = this_cpu()->current;
        uint64_t got;

        spin_lock(&self->lock);
        got = self->sig_received & mask;
        if (atomic_load(&self->killed) || got) {
            /* Killed: back out of the kernel call; user_return() ends it. */
            self->sig_received &= ~got;
            spin_unlock(&self->lock);
            arch_irq_restore(was);
            if (loan)
                sched_kick(loan);
            return got;
        }
        self->sig_waiting = mask | SIGF_KILL;
        spin_lock(&self->cpu->rq_lock);
        self->state = TASK_WAITING;
        spin_unlock(&self->cpu->rq_lock);
        spin_unlock(&self->lock);
        loan = nullptr;
        schedule();
        arch_irq_restore(was);
    }
}

struct cpu *task_post(struct task *t, uint64_t mask, bool handoff)
{
    unsigned long was = spin_lock_irqsave(&t->lock);
    struct cpu *self = this_cpu(), *c = nullptr;
    struct task *me = self->current;
    bool notify = false;

    t->sig_received |= mask;
    if (t->sig_received & t->sig_waiting) {
        c = t->cpu;
        spin_lock(&c->rq_lock);
        if (t->state == TASK_WAITING) {
            t->sig_waiting = 0;
            t->state = TASK_READY;
            if (handoff && !t->on_rq && (t->affinity & bit(self)) && me != self->idle) {
                /* Here, next: the caller is about to wait for it. Its
                 * state keeps everyone else off while it moves (they
                 * take t->lock first anyway). */
                if (c != self) {
                    spin_unlock(&c->rq_lock);
                    c = self;
                    spin_lock(&c->rq_lock);
                }
                if (t->pri < me->pri)
                    t->pri = me->pri;   /* lent until it next signals or waits */
                enqueue_head(c, t);
                atomic_fetch_add_explicit(&handoffs, 1, memory_order_relaxed);
            } else if (!t->on_rq) {
                enqueue(c, t);
            }
            notify = wake_check(c, t);
        }
        spin_unlock(&c->rq_lock);
    }
    spin_unlock_irqrestore(&t->lock, was);
    return notify ? c : nullptr;
}

void task_signal(struct task *t, uint64_t mask)
{
    struct cpu *c = task_post(t, mask, false);

    if (c)
        sched_kick(c);
}

uint64_t task_set_signal(uint64_t bits, uint64_t mask)
{
    struct task *self = task_self();
    unsigned long was = spin_lock_irqsave(&self->lock);
    uint64_t old = self->sig_received;

    self->sig_received = (old & ~mask) | (bits & mask);
    spin_unlock_irqrestore(&self->lock, was);
    return old;
}

void task_yield(void)
{
    schedule();
}

static void sleep_fire(void *data)
{
    task_signal(data, SIGF_SLEEP);
}

void task_sleep_until(uint64_t deadline)
{
    struct task *self = task_self();

    if (time_now() >= deadline)
        return;
    task_set_signal(0, SIGF_SLEEP);
    self->sleep = (struct ktimer){ .fn = sleep_fire, .data = self };
    ktimer_start(&self->sleep, deadline);
    task_wait(SIGF_SLEEP);
}

uint64_t sched_switches(void)
{
    return atomic_load(&switches);
}

uint64_t sched_steals(void)
{
    return atomic_load(&steals);
}

uint64_t sched_handoffs(void)
{
    return atomic_load(&handoffs);
}
