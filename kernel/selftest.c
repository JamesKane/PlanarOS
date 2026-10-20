/*
 * kernel/selftest.c -- boot-time self-tests
 */
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "ktime.h"
#include "pmm.h"
#include "handle.h"
#include "process.h"
#include "syscall.h"
#include "userimg.h"
#include "sched.h"
#include "selftest.h"

[[noreturn]] static void fail(const char *test, const char *why)
{
    kprintf("%s FAIL: %s\n", test, why);
    arch_halt();
}

/*
 * The page allocator: pages are distinct, zeroed, writable through the
 * direct map, and all come back; a contiguous run is contiguous and
 * comes back too. Fresh memory is often zero already, so zeroing is
 * checked on pages handed out again after being dirtied and freed.
 */
void selftest_pmm(void)
{
    constexpr int N = 64;
    static uint64_t pages[N];
    uint64_t before = pmm_free_pages(), run;

    if (pmm_total() == 0) {
        kprintf("V1.1 pmm skipped: no managed memory\n");
        return;
    }
    for (int i = 0; i < N; i++) {
        uint64_t *p;

        if (!(pages[i] = pmm_alloc()))
            fail("V1.1 pmm", "allocation failed");
        p = phys_to_virt(pages[i]);
        for (uint64_t w = 0; w < PAGE_SIZE / 8; w++)
            if (p[w])
                fail("V1.1 pmm", "page not zeroed");
        p[0] = pages[i];
        for (int j = 0; j < i; j++)
            if (pages[j] == pages[i])
                fail("V1.1 pmm", "page handed out twice");
    }
    for (int i = 0; i < N; i++)
        if (*(uint64_t *)phys_to_virt(pages[i]) != pages[i])
            fail("V1.1 pmm", "page contents lost");
    if (pmm_free_pages() != before - N)
        fail("V1.1 pmm", "free count wrong after allocation");
    for (int i = 0; i < N; i++)
        pmm_free(pages[i]);
    int reused = 0;
    for (int i = 0; i < N; i++) {
        uint64_t page = pmm_alloc(), *p = phys_to_virt(page);

        for (int j = 0; j < N; j++)
            reused += pages[j] == page;
        for (uint64_t w = 0; w < PAGE_SIZE / 8; w++)
            if (p[w])
                fail("V1.1 pmm", "reused page not zeroed");
        pages[i] = page;
    }
    if (!reused)
        fail("V1.1 pmm", "no freed page was handed out again");
    for (int i = 0; i < N; i++)
        pmm_free(pages[i]);

    if (!(run = pmm_alloc_contig(16)))
        fail("V1.1 pmm", "contiguous allocation failed");
    for (uint64_t i = 0; i < 16; i++)
        *(volatile uint64_t *)phys_to_virt(run + i * PAGE_SIZE) = i;
    for (uint64_t i = 0; i < 16; i++)
        pmm_free(run + i * PAGE_SIZE);
    if (pmm_free_pages() != before)
        fail("V1.1 pmm", "pages leaked");
    kprintf("V1.1 pmm ok: %llu pages (%llu MiB) managed, %llu free, largest run %llu\n",
            (unsigned long long)pmm_total(), (unsigned long long)(pmm_total() >> 8),
            (unsigned long long)before, (unsigned long long)pmm_largest_free_run());
}

/*
 * Fault recovery: a read of a mapped kernel address succeeds through the
 * probe, and a read of an unmapped one (user page 1, with no user domain
 * loaded) faults into the fixup instead of a Guru Meditation.
 */
void selftest_fault(void)
{
    static const uint64_t marker = 0x5ec7a5ec7a5ec7aULL;
    uint64_t v = 0;

    if (!arch_probe_read((uint64_t)(uintptr_t)&marker, &v)) {
        kprintf("V1.1 fault skipped: no fault recovery on %s\n", arch_name);
        return;
    }
    if (v != marker)
        fail("V1.1 fault", "probe read the wrong value");
    if (arch_probe_read(0x1000, &v))
        fail("V1.1 fault", "unmapped read didn't fault");
    kprintf("V1.1 fault ok\n");
}

static void deadline_fn(void *)
{
}

/*
 * Sleep this CPU until cond(arg) holds or 'us' pass; true if it holds.
 * The check runs with interrupts masked, so an interrupt that makes it
 * true can't slip in between the check and the wait and leave the CPU
 * asleep (wfi still wakes for a masked interrupt). A deadline timer
 * makes sure something wakes it.
 */
static bool wait_for(bool (*cond)(void *), void *arg, uint64_t us)
{
    struct ktimer deadline = { .fn = deadline_fn };
    uint64_t end = time_now() + time_us(us);
    bool ok;

    ktimer_start(&deadline, end);
    for (;;) {
        arch_irq_disable();
        if ((ok = cond(arg)) || time_now() >= end) {
            arch_irq_enable();
            break;
        }
        arch_wait_for_interrupt();
        arch_irq_enable();
    }
    ktimer_stop(&deadline);
    return ok;
}

static volatile int fired;

static bool timer_fired(void *)
{
    return fired != 0;
}

static void timer_fn(void *)
{
    fired++;
}

/*
 * The timer: a kernel timer 10 ms out fires once, not early, and through
 * an interrupt rather than polling.
 */
void selftest_timer(void)
{
    static struct ktimer t = { .fn = timer_fn };
    uint64_t start, interrupts = time_interrupts(), deadline, waited;

    if (!time_have_timers()) {
        kprintf("V1.1 timer skipped: no timer interrupt on %s\n", arch_name);
        return;
    }
    start = time_now();
    deadline = start + time_us(10000);
    ktimer_start(&t, deadline);
    wait_for(timer_fired, nullptr, 500000);
    waited = time_now() - start;
    if (!fired)
        fail("V1.1 timer", "timer never fired");
    if (fired != 1 || t.pending)
        fail("V1.1 timer", "timer fired more than once");
    if (time_now() < deadline)
        fail("V1.1 timer", "timer fired early");
    if (time_interrupts() == interrupts)
        fail("V1.1 timer", "no timer interrupt taken");
    kprintf("V1.1 timer ok: %s at %llu Hz, 10 ms timer took %llu us\n", arch_timer_name(),
            (unsigned long long)time_hz(), (unsigned long long)time_to_us(waited));
}

/* Every CPU the firmware describes came online, under its own ID. */
void selftest_smp(void)
{
    if (ncpus == 1) {
        kprintf("V1.2 smp skipped: one CPU\n");
        return;
    }
    for (unsigned i = 0; i < ncpus; i++)
        if (!atomic_load(&cpus[i].online))
            fail("V1.2 smp", "a CPU didn't come online");
    kprintf("V1.2 smp ok: %u CPUs online:", cpus_online());
    for (unsigned i = 0; i < ncpus; i++)
        kprintf(" %llx", (unsigned long long)cpus[i].hwid);
    kprintf("\n");
}

static void record_cpu(void *arg)
{
    *(unsigned *)arg = this_cpu()->index;
}

/*
 * Cross-CPU calls: each one runs on the CPU it was sent to, through an
 * IPI that CPU took. Also times the round trip (send, run, see done).
 */
void selftest_ipi(void)
{
    constexpr int ROUNDS = 100;
    uint64_t total = 0;

    if (ncpus == 1) {
        kprintf("V1.2 ipi skipped: one CPU\n");
        return;
    }
    for (unsigned i = 1; i < ncpus; i++) {
        unsigned ran_on = ~0u;
        uint64_t ipis = atomic_load(&cpus[i].ipis);
        struct cpu_call call = { .fn = record_cpu, .arg = &ran_on };

        uint64_t start = time_now();

        /* The first call waits with a timeout, so a lost IPI fails the
         * test instead of hanging it. */
        if (!cpu_call(&cpus[i], &call, false))
            fail("V1.2 ipi", "call refused");
        while (!atomic_load(&call.done) && time_now() - start < time_hz())
            arch_cpu_relax();
        if (!atomic_load(&call.done))
            fail("V1.2 ipi", "call never ran");
        if (ran_on != i)
            fail("V1.2 ipi", "call ran on the wrong CPU");
        if (atomic_load(&cpus[i].ipis) == ipis)
            fail("V1.2 ipi", "no IPI taken");
        for (int r = 0; r < ROUNDS; r++) {
            uint64_t t0 = time_now();

            cpu_call(&cpus[i], &call, true);
            total += time_now() - t0;
        }
    }
    kprintf("V1.2 ipi ok: round trip %llu ns on average\n",
            (unsigned long long)(time_to_us(total * 1000) / (ROUNDS * (ncpus - 1))));
}

static struct ktimer cpu_timers[MAX_CPUS];
static atomic_uint fired_on[MAX_CPUS];

static void cpu_timer_fn(void *arg)
{
    unsigned i = (unsigned)(uintptr_t)arg;

    atomic_store(&fired_on[i], this_cpu()->index + 1);
}

static bool cpu_timer_fired(void *arg)
{
    return atomic_load(&fired_on[(unsigned)(uintptr_t)arg]) != 0;
}

static void start_cpu_timer(void *)
{
    unsigned i = this_cpu()->index;

    cpu_timers[i] = (struct ktimer){ .fn = cpu_timer_fn, .data = (void *)(uintptr_t)i };
    ktimer_start(&cpu_timers[i], time_now() + time_us(2000));
}

/* Each CPU's own timer: a 2 ms timer started on a CPU fires there. */
void selftest_cpu_timers(void)
{
    if (ncpus == 1 || !time_have_timers()) {
        kprintf("V1.2 cpu timers skipped: %s\n", ncpus == 1 ? "one CPU" : "no timers");
        return;
    }
    for (unsigned i = 0; i < ncpus; i++) {
        struct cpu_call call = { .fn = start_cpu_timer };

        atomic_store(&fired_on[i], 0);
        cpu_call(&cpus[i], &call, true);
    }
    for (unsigned i = 0; i < ncpus; i++) {
        if (!wait_for(cpu_timer_fired, (void *)(uintptr_t)i, 500000))
            fail("V1.2 cpu timers", "a CPU's timer never fired");
        if (atomic_load(&fired_on[i]) != i + 1)
            fail("V1.2 cpu timers", "a timer fired on the wrong CPU");
    }
    kprintf("V1.2 cpu timers ok: %u CPUs\n", ncpus);
}

static spinlock_t test_lock;
static uint64_t counter;                /* only under test_lock */
static atomic_uint lock_done;
constexpr unsigned LOCK_ROUNDS = 20000;

static void hammer(void *)
{
    for (unsigned n = 0; n < LOCK_ROUNDS; n++) {
        spin_lock(&test_lock);
        counter++;
        spin_unlock(&test_lock);
    }
    atomic_fetch_add(&lock_done, 1);
}

/* A spinlock under contention from every CPU: no increment is lost. */
void selftest_lock(void)
{
    static struct cpu_call calls[MAX_CPUS];
    uint64_t start = time_now();

    counter = 0;
    atomic_store(&lock_done, 0);
    for (unsigned i = 1; i < ncpus; i++) {
        calls[i] = (struct cpu_call){ .fn = hammer };
        cpu_call(&cpus[i], &calls[i], false);
    }
    hammer(nullptr);
    while (atomic_load(&lock_done) < ncpus && time_now() - start < 10 * time_hz())
        arch_cpu_relax();
    if (atomic_load(&lock_done) < ncpus)
        fail("V1.2 lock", "a CPU never finished");
    if (counter != (uint64_t)ncpus * LOCK_ROUNDS)
        fail("V1.2 lock", "increments lost");
    kprintf("V1.2 lock ok: %u CPUs x %u rounds\n", ncpus, LOCK_ROUNDS);
}

/* ---------------------------------------------------------------------
 * V1 step 3: tasks and the scheduler. These run in the "selftest" task
 * (priority 10, on the boot CPU), so they may block.
 */

constexpr unsigned SIGB_TEST = 16;              /* the first non-system signal */
constexpr uint64_t SIGF_TEST = 1ULL << SIGB_TEST;

static struct task *tester;

static void timeout_fire(void *data)
{
    task_signal(data, SIGF_SLEEP);
}

/* Wait for SIGF_TEST for at most 'us': false if it timed out, so a
 * broken scheduler fails a test instead of hanging it. */
static bool wait_test(uint64_t us)
{
    struct task *self = task_self();
    uint64_t got;

    task_set_signal(0, SIGF_SLEEP);
    self->sleep = (struct ktimer){ .fn = timeout_fire, .data = self };
    ktimer_start(&self->sleep, time_now() + time_us(us));
    got = task_wait(SIGF_TEST | SIGF_SLEEP);
    ktimer_stop(&self->sleep);
    if (got & SIGF_SLEEP && !(got & SIGF_TEST))
        return false;
    task_set_signal(0, SIGF_SLEEP);
    return true;
}

static uint64_t cpu_mask(unsigned i)
{
    return 1ULL << (i % ncpus);
}

/* --- many tasks: created, run, exit, and their memory comes back --- */

constexpr unsigned SPAWN = 24;
static atomic_uint spawned_done;
static atomic_uint_fast64_t ran_on_mask;

static void spawn_fn(void *)
{
    atomic_fetch_or(&ran_on_mask, 1ULL << this_cpu()->index);
    if (atomic_fetch_add(&spawned_done, 1) + 1 == SPAWN)
        task_signal(tester, SIGF_TEST);
}

static void test_spawn(void)
{
    uint64_t free_before = pmm_free_pages(), start;
    unsigned cpus_used;

    atomic_store(&spawned_done, 0);
    atomic_store(&ran_on_mask, 0);
    for (unsigned i = 0; i < SPAWN; i++)
        if (!task_create("spawn", 5, TASK_ANY_CPU, spawn_fn, nullptr))
            fail("V1.3 tasks", "task_create failed");
    if (!wait_test(1000000) || atomic_load(&spawned_done) != SPAWN)
        fail("V1.3 tasks", "not every task ran");
    /* Dead tasks are freed by the next switch on their CPU. */
    start = time_now();
    while (pmm_free_pages() != free_before && time_now() - start < time_hz())
        task_sleep_until(time_now() + time_us(1000));
    if (pmm_free_pages() != free_before)
        fail("V1.3 tasks", "dead tasks' memory not freed");
    cpus_used = (unsigned)__builtin_popcountll(atomic_load(&ran_on_mask));
    if (ncpus > 1 && cpus_used < 2)
        fail("V1.3 tasks", "every task ran on one CPU");
    kprintf("V1.3 tasks ok: %u tasks over %u CPUs, all memory back\n", SPAWN, cpus_used);
}

/* --- signals: masks, pending bits, SetSignal --- */

static void test_signals(void)
{
    uint64_t got;

    task_set_signal(0, ~0ULL);
    task_signal(task_self(), 1ULL << 20 | 1ULL << 21);
    got = task_wait(1ULL << 20);
    if (got != 1ULL << 20)
        fail("V1.3 signals", "wait returned the wrong bits");
    if (task_set_signal(0, 0) != 1ULL << 21)
        fail("V1.3 signals", "a bit outside the mask was lost");
    if (task_set_signal(0, 1ULL << 21) != 1ULL << 21 || task_set_signal(0, 0) != 0)
        fail("V1.3 signals", "SetSignal didn't clear");
    kprintf("V1.3 signals ok\n");
}

/* --- preemption: a higher-priority task runs at once on a busy CPU --- */

static atomic_bool spin_stop;
static atomic_uint_fast64_t spin_counts[2];
static atomic_uint_fast64_t woke_at;
static atomic_uint spin_finished;

/* Signal bits don't count, so finishers count themselves. */
static void spinner(void *arg)
{
    unsigned slot = (unsigned)(uintptr_t)arg;

    while (!atomic_load(&spin_stop))
        atomic_fetch_add(&spin_counts[slot], 1);
    atomic_fetch_add(&spin_finished, 1);
    task_signal(tester, SIGF_TEST);
}

static void stop_spinners(unsigned n)
{
    atomic_store(&spin_stop, true);
    while (atomic_load(&spin_finished) < n)
        task_wait(SIGF_TEST);
    task_set_signal(0, SIGF_TEST);
}

static void urgent(void *)
{
    atomic_store(&woke_at, time_now());
    task_signal(tester, SIGF_TEST);
}

static void test_preempt(void)
{
    unsigned target = ncpus > 1 ? 1 : 0;
    uint64_t t0, waited;

    if (ncpus == 1) {
        kprintf("V1.3 preempt skipped: one CPU\n");
        return;
    }
    atomic_store(&spin_stop, false);
    atomic_store(&spin_finished, 0);
    atomic_store(&spin_counts[0], 0);
    task_create("spinner", 1, cpu_mask(target), spinner, (void *)0);
    while (atomic_load(&spin_counts[0]) == 0)
        task_sleep_until(time_now() + time_us(1000));
    atomic_store(&woke_at, 0);
    t0 = time_now();
    task_create("urgent", 20, cpu_mask(target), urgent, nullptr);
    if (!wait_test(500000))
        fail("V1.3 preempt", "higher-priority task never ran on the busy CPU");
    waited = atomic_load(&woke_at) - t0;
    if (atomic_load(&spin_finished))
        fail("V1.3 preempt", "spinner stopped early");
    stop_spinners(1);
    kprintf("V1.3 preempt ok: higher priority ran on a busy CPU after %llu us\n",
            (unsigned long long)time_to_us(waited));
}

/*
 * --- round robin: two equal-priority spinners on one CPU take turns ---
 * Both making progress isn't enough: one stray switch would do that.
 * Over 15 quanta each should be switched in several times.
 */

static void test_round_robin(void)
{
    unsigned target = ncpus > 1 ? ncpus - 1 : 0;
    uint64_t a, b, a_runs, b_runs;
    struct task *ta, *tb;

    if (ncpus == 1) {
        kprintf("V1.3 round robin skipped: one CPU\n");
        return;
    }
    atomic_store(&spin_stop, false);
    atomic_store(&spin_finished, 0);
    atomic_store(&spin_counts[0], 0);
    atomic_store(&spin_counts[1], 0);
    ta = task_create("rr-a", 3, cpu_mask(target), spinner, (void *)0);
    tb = task_create("rr-b", 3, cpu_mask(target), spinner, (void *)1);
    if (!ta || !tb)
        fail("V1.3 round robin", "task_create failed");
    task_sleep_until(time_now() + time_us(60000));      /* 15 quanta */
    a = atomic_load(&spin_counts[0]);
    b = atomic_load(&spin_counts[1]);
    a_runs = ta->runs;                  /* while they're still alive */
    b_runs = tb->runs;
    stop_spinners(2);
    if (!a || !b)
        fail("V1.3 round robin", "a spinner never ran");
    if (a_runs < 3 || b_runs < 3)
        fail("V1.3 round robin", "spinners didn't take turns");
    kprintf("V1.3 round robin ok: %llu and %llu turns, %llu and %llu iterations in 60 ms\n",
            (unsigned long long)a_runs, (unsigned long long)b_runs,
            (unsigned long long)a, (unsigned long long)b);
}

/* --- migration: a task moves itself to another CPU --- */

static atomic_uint moved_from, moved_to;

static void mover(void *)
{
    atomic_store(&moved_from, this_cpu()->index + 1);
    task_set_affinity(cpu_mask(this_cpu()->index + 1));
    atomic_store(&moved_to, this_cpu()->index + 1);
    task_signal(tester, SIGF_TEST);
}

static void test_migrate(void)
{
    unsigned from, to;

    if (ncpus == 1) {
        kprintf("V1.3 migrate skipped: one CPU\n");
        return;
    }
    task_create("mover", 5, cpu_mask(1), mover, nullptr);
    if (!wait_test(500000))
        fail("V1.3 migrate", "mover never finished");
    from = atomic_load(&moved_from) - 1;
    to = atomic_load(&moved_to) - 1;
    if (from != 1 || to != (2 % ncpus))
        fail("V1.3 migrate", "task didn't end up on its new CPU");
    kprintf("V1.3 migrate ok: cpu %u to cpu %u\n", from, to);
}

/* --- ping-pong: the cost of Signal + Wait, on one CPU and across two --- */

constexpr unsigned PINGS = 2000;
static struct task *pong_task;

static void ponger(void *)
{
    for (unsigned n = 0; n < PINGS; n++) {
        task_wait(SIGF_TEST);
        task_signal(tester, SIGF_TEST);
    }
}

static void pingpong(const char *what, unsigned ping_cpu, unsigned pong_cpu)
{
    uint64_t t0, c0, ticks, cycles;

    task_set_affinity(cpu_mask(ping_cpu));
    pong_task = task_create("pong", 10, cpu_mask(pong_cpu), ponger, nullptr);
    if (!pong_task)
        fail("V1.3 pingpong", "task_create failed");
    task_sleep_until(time_now() + time_us(2000));       /* let it reach its Wait */
    t0 = time_now();
    c0 = arch_read_cycles();
    for (unsigned n = 0; n < PINGS; n++) {
        task_signal(pong_task, SIGF_TEST);
        task_wait(SIGF_TEST);
    }
    cycles = arch_read_cycles() - c0;
    ticks = time_now() - t0;
    task_set_affinity(1);
    kprintf("V1.3 pingpong ok: %s: round trip %llu ns, %llu cycles\n", what,
            (unsigned long long)(time_to_us(ticks * 1000) / PINGS),
            (unsigned long long)(cycles / PINGS));
}

static void test_pingpong(void)
{
    uint64_t switches = sched_switches();

    pingpong("same CPU (2 switches)", ncpus > 1 ? 1 : 0, ncpus > 1 ? 1 : 0);
    if (sched_switches() - switches < 2 * PINGS)
        fail("V1.3 pingpong", "fewer switches than round trips");
    if (ncpus > 2)
        pingpong("across CPUs (Signal + IPI + wake)", 1, 2);
}

void selftest_tasks(void *)
{
    tester = task_self();
    test_spawn();
    test_signals();
    test_preempt();
    test_round_robin();
    test_migrate();
    test_pingpong();
    kprintf("V1.3 scheduler: %llu switches, %llu steals so far\n",
            (unsigned long long)sched_switches(), (unsigned long long)sched_steals());
    kprintf("V1.3 done\n");
    selftest_user();
    kprintf("V1.4 done\n");
}

/* ---------------------------------------------------------------------
 * V1 step 4: processes in user mode, run from the selftest task.
 */

constexpr unsigned SIGB_DEAD = 17;
constexpr uint64_t SIGF_DEAD = 1ULL << SIGB_DEAD;

/* Run built-in program 'name' and wait (at most 'us') for it to die. */
static struct process *run(const char *name, uint64_t arg, uint64_t us)
{
    struct process *p;
    uint64_t start = time_now();

    task_set_signal(0, SIGF_DEAD);
    if (!(p = process_create(name, 5, arg, task_self(), SIGF_DEAD)))
        fail("V1.4 user", "process_create failed");
    while (!atomic_load(&p->dead) && time_now() - start < time_us(us)) {
        struct ktimer t = { .fn = timeout_fire, .data = task_self() };

        ktimer_start(&t, time_now() + time_us(1000));
        task_wait(SIGF_DEAD | SIGF_SLEEP);
        ktimer_stop(&t);
    }
    return p;
}

static void expect_exit(const char *name, int64_t code)
{
    struct process *p = run(name, 0, 1000000);

    if (!atomic_load(&p->dead))
        fail("V1.4 user", "process never ended");
    if (p->fault != FAULT_NONE || p->exit_code != code)
        fail("V1.4 user", "wrong exit");
    process_free(p);
}

static void expect_fault(const char *name, uint64_t arg, enum fault kind, uint64_t addr)
{
    struct process *p = run(name, arg, 1000000);

    if (!atomic_load(&p->dead))
        fail("V1.4 fault", "faulting process never ended");
    if (p->fault != kind)
        fail("V1.4 fault", "wrong kind of fault");
    if (addr && p->fault_addr != addr)
        fail("V1.4 fault", "wrong fault address");
    process_free(p);
}

static void wait_test_dead(void)
{
    struct ktimer t = { .fn = timeout_fire, .data = task_self() };

    ktimer_start(&t, time_now() + time_us(1000));
    task_wait(SIGF_DEAD | SIGF_SLEEP);
    ktimer_stop(&t);
}

/* Kill a process once it's well under way; it must be dead soon after.
 * Returns how long that took, in microseconds. */
static uint64_t expect_killed(const char *name)
{
    struct process *p;
    uint64_t start;

    task_set_signal(0, SIGF_DEAD);
    if (!(p = process_create(name, 5, 0, task_self(), SIGF_DEAD)))
        fail("V1.4 kill", "process_create failed");
    task_sleep_until(time_now() + time_us(20000));
    if (atomic_load(&p->dead))
        fail("V1.4 kill", "process ended by itself");
    start = time_now();
    process_kill(p);
    while (!atomic_load(&p->dead) && time_now() - start < time_hz())
        wait_test_dead();
    if (!atomic_load(&p->dead))
        fail("V1.4 kill", "killed process never ended");
    start = time_now() - start;
    process_free(p);
    return time_to_us(start);
}

/*
 * FP and vector state: two copies of fpu (three tasks each, more tasks
 * than CPUs) hold all their registers through preemption and moves
 * between CPUs; the kernel switches the state lazily.
 */
static void test_fpu(void)
{
    struct process *p[2];
    struct arch_lazy_counts before, after;
    uint64_t start = time_now();

    arch_lazy_counts(&before);
    task_set_signal(0, SIGF_DEAD);
    for (int i = 0; i < 2; i++)
        if (!(p[i] = process_create("fpu", 5, time_us(10000), task_self(), SIGF_DEAD)))
            fail("V1.4 fpu", "process_create failed");
    while ((!atomic_load(&p[0]->dead) || !atomic_load(&p[1]->dead)) &&
           time_now() - start < 10 * time_hz())
        wait_test_dead();
    for (int i = 0; i < 2; i++) {
        if (!atomic_load(&p[i]->dead))
            fail("V1.4 fpu", "fpu never finished");
        if (p[i]->fault != FAULT_NONE)
            fail("V1.4 fpu", "fpu faulted");
        if (p[i]->exit_code != 0)
            fail("V1.4 fpu", "registers changed under a task");
        process_free(p[i]);
    }
    arch_lazy_counts(&after);
    if (after.first_uses == before.first_uses || after.loads == before.loads)
        fail("V1.4 fpu", "state was never switched lazily");
    kprintf("V1.4 fpu ok: 6 tasks kept their registers; %llu first uses, %llu loads, "
            "%llu saves, %llu returns found their state still loaded\n",
            (unsigned long long)(after.first_uses - before.first_uses),
            (unsigned long long)(after.loads - before.loads),
            (unsigned long long)(after.saves - before.saves),
            (unsigned long long)(after.reuses - before.reuses));
}

void selftest_user(void)
{
    uint64_t free_before = pmm_free_pages(), spin_us, wait_us;
    unsigned handles_before = handle_count(), domains_before = arch_domains_live();
    uint64_t kernel_page = pmm_alloc();

    if (!user_images[0].name) {
        kprintf("V1.4 user skipped: no user programs\n");
        return;
    }
    expect_exit("hello", 7);
    expect_exit("tasks", 0);
    kprintf("V1.4 user ok: hello exited 7; tasks: 4 tasks in one process\n");

    expect_fault("wild", 0, FAULT_WRITE, 0x10);
    expect_fault("wx", 0, FAULT_WRITE, 0);
    expect_fault("peek", (uint64_t)(uintptr_t)phys_to_virt(kernel_page), FAULT_READ,
                 (uint64_t)(uintptr_t)phys_to_virt(kernel_page));
    pmm_free(kernel_page);
    kprintf("V1.4 fault ok: wild write, write to code, read of the kernel each ended "
            "only their process\n");

    spin_us = expect_killed("spin");
    wait_us = expect_killed("waiter");
    kprintf("V1.4 kill ok: 3 spinning tasks dead %llu us after the kill, a blocked one %llu us\n",
            (unsigned long long)spin_us, (unsigned long long)wait_us);

    test_fpu();

    /* Reclamation is asynchronous only by a switch; give it a moment. */
    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() != free_before)
        fail("V1.4 reclaim", "pages leaked");
    if (handle_count() != handles_before)
        fail("V1.4 reclaim", "handles leaked");
    if (arch_domains_live() != domains_before)
        fail("V1.4 reclaim", "ASIDs leaked");
    if (process_count() != 0)
        fail("V1.4 reclaim", "process records leaked");
    kprintf("V1.4 reclaim ok: every page, handle and ASID back; %llu kernel calls made\n",
            (unsigned long long)syscall_count());
}
