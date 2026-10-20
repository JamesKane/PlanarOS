/*
 * kernel/selftest.c -- boot-time self-tests
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "ktime.h"
#include "plat.h"
#include "pmm.h"
#include "area.h"
#include "bootargs.h"
#include "handle.h"
#include "kmalloc.h"
#include "layout.h"
#include "port.h"
#include "process.h"
#include "syscall.h"
#include "userimg.h"
#include "va.h"
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
static atomic_bool spawn_go;

/* Each spins until all are created, so they're alive together: a task
 * that ended before the next was made would leave its CPU idle again,
 * and placement would choose it every time. */
static void spawn_fn(void *)
{
    while (!atomic_load(&spawn_go))
        arch_cpu_relax();
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
    atomic_store(&spawn_go, false);
    for (unsigned i = 0; i < SPAWN; i++)
        if (!task_create("spawn", 5, TASK_ANY_CPU, spawn_fn, nullptr))
            fail("V1.3 tasks", "task_create failed");
    atomic_store(&spawn_go, true);
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
    selftest_areas();
    selftest_vq3();
    kprintf("V1.5 done\n");
    selftest_ipc();
    kprintf("V2.1 done\n");
    if (bootargs_has("hang")) {
        /* For checking a board's watchdog brings it back. */
        kprintf("planar: hanging, interrupts off\n");
        task_sleep_until(time_now() + time_us(100000));
        arch_halt();
    }
    if (bootargs_has("reboot")) {
        /* Let the serial line drain, then start over (a board's loop). */
        task_sleep_until(time_now() + time_us(200000));
        kprintf("planar: rebooting\n");
        task_sleep_until(time_now() + time_us(50000));
        plat_reboot();
        arch_reboot();      /* says so if the firmware can't */
    }
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
    uint64_t free_before = pmm_free_pages(), heap_before = kmalloc_pages(), spin_us, wait_us;
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
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
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

/* ---------------------------------------------------------------------
 * V1 step 5: Areas, run from the selftest task.
 */

/* Two processes share an Area: same address, same page, in both; it
 * lives while either maps it and goes when the last one does. */
static void test_share(void)
{
    struct process *p1, *p2;
    struct area *a;
    uint64_t va;
    unsigned areas_before = area_count(), with_processes;

    task_set_signal(0, SIGF_DEAD);
    p1 = process_create("waiter", 5, 0, task_self(), SIGF_DEAD);
    p2 = process_create("waiter", 5, 0, task_self(), SIGF_DEAD);
    if (!p1 || !p2)
        fail("V1.5 share", "process_create failed");
    with_processes = area_count();      /* their images and stacks */
    if (!(a = area_new(layout_boot(), 3, 0, false)))
        fail("V1.5 share", "area_new failed");
    va = area_base(a);
    if (!area_map(a, p1, AREAR_READ | AREAR_WRITE, 0) || area_share(a, p2, AREAR_READ) != 0)
        fail("V1.5 share", "mapping failed");
    if (area_share(a, p2, AREAR_READ) != ERR_EXISTS)
        fail("V1.5 share", "shared twice");
    for (uint64_t off = 0; off < 3 * PAGE_SIZE; off += PAGE_SIZE)
        if (arch_lookup(&p1->domain, va + off) != area_pa(a, off) ||
            arch_lookup(&p2->domain, va + off) != area_pa(a, off))
            fail("V1.5 share", "not the same pages at the same address");
    area_release(a, p1);
    if (arch_lookup(&p1->domain, va) || arch_lookup(&p2->domain, va) != area_pa(a, 0) ||
        area_count() != with_processes + 1)
        fail("V1.5 share", "release took the Area from the wrong process");
    process_kill(p1);
    process_kill(p2);
    while (!atomic_load(&p1->dead) || !atomic_load(&p2->dead))
        wait_test_dead();
    process_free(p1);
    process_free(p2);
    if (area_count() != areas_before)
        fail("V1.5 share", "the Area outlived everyone mapping it");
    kprintf("V1.5 share ok: one Area at %p in two processes, freed with the last\n",
            (void *)va);
}

void selftest_areas(void)
{
    uint64_t free_before = pmm_free_pages(), heap_before = kmalloc_pages();
    uint64_t va_before = va_free_bytes();
    struct process *p;

    if (!user_images[0].name) {
        kprintf("V1.5 areas skipped: no user programs\n");
        return;
    }
    expect_exit("areas", 0);
    kprintf("V1.5 areas ok (%s layout): allocate, query, protect, run written code, "
            "free, churn\n", layout_name(layout_boot()));

    p = run("overrun", 0, 1000000);
    /* In the guard page, and the guard unmapped: a page below that is
     * merely read-only (code, say) would fault too, and prove nothing. */
    if (!atomic_load(&p->dead) || p->fault != FAULT_WRITE || p->fault_mapped ||
        p->fault_addr >= p->stack_base || p->fault_addr < p->stack_base - PAGE_SIZE)
        fail("V1.5 guard", "stack overrun didn't fault in an unmapped guard page");
    kprintf("V1.5 guard ok: overrun faulted at %p, just below its stack at %p\n",
            (void *)p->fault_addr, (void *)p->stack_base);
    process_free(p);
    expect_fault("rofault", 0, FAULT_WRITE, 0);
    kprintf("V1.5 rights ok: a write to a read-only Area faulted\n");

    test_share();

    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
        fail("V1.5 reclaim", "pages leaked");
    if (area_count() != 0)
        fail("V1.5 reclaim", "Areas leaked");
    if (va_free_bytes() != va_before)
        fail("V1.5 reclaim", "address space leaked");
    kprintf("V1.5 reclaim ok: every page, Area and address back\n");
}

/* ---------------------------------------------------------------------
 * The VQ3 soak: the same random workload against both layouts.
 */

constexpr unsigned SOAK_OPS = 6000;
constexpr unsigned SOAK_HELD = 1024;
constexpr unsigned SOAK_PROC = 32;      /* Areas per measured "process" */

static uint64_t soak_rng;

static uint64_t soak_random(void)
{
    soak_rng ^= soak_rng << 13;
    soak_rng ^= soak_rng >> 7;
    soak_rng ^= soak_rng << 17;
    return soak_rng;
}

/* Mostly small, some medium, a few large (4 MiB on a 256 MiB machine),
 * scaled with memory so a big machine sees the same pressure: heaps,
 * buffers, frames. */
static uint64_t soak_scale;

static uint64_t soak_size(void)
{
    uint64_t r = soak_random() % 100;

    if (r < 70)
        return (1 + soak_random() % 8) * soak_scale;
    if (r < 95)
        return (9 + soak_random() % 120) * soak_scale;
    return (129 + soak_random() % 896) * soak_scale;
}

struct soak_result {
    uint64_t attempts, failures, needless, alloc_ticks, peak_pages, largest_run;
    uint64_t tables, domains, guarded;
};

static void soak(enum layout l, struct soak_result *r)
{
    static struct area *held[SOAK_HELD];
    static uint64_t held_pages[SOAK_HELD];
    unsigned nheld = 0;
    uint64_t in_use = 0, target = pmm_free_pages() * 9 / 10;

    *r = (struct soak_result){ 0 };
    soak_rng = 0x9e3779b97f4a7c15ULL;       /* the same workload for both */
    soak_scale = pmm_total() / 65536 ? pmm_total() / 65536 : 1;
    for (unsigned op = 0; op < SOAK_OPS; op++) {
        bool grow = nheld == 0 || (nheld < SOAK_HELD && in_use < target && soak_random() % 3);

        if (grow) {
            uint64_t n = soak_size(), t0 = time_now();
            bool guard = soak_random() % 8 == 0;    /* a stack */
            struct area *a = area_new(l, n, 0, guard);

            r->alloc_ticks += time_now() - t0;
            r->attempts++;
            if (!a) {
                r->failures++;
                if (pmm_free_pages() >= n + guard)
                    r->needless++;      /* enough memory, but not in one piece */
                continue;
            }
            r->guarded += guard;
            held[nheld] = a;
            held_pages[nheld++] = n;
            in_use += n;
            if (in_use > r->peak_pages)
                r->peak_pages = in_use;
        } else {
            unsigned i = (unsigned)(soak_random() % nheld);

            area_destroy(held[i]);
            in_use -= held_pages[i];
            held[i] = held[--nheld];
            held_pages[i] = held_pages[nheld];
        }
        /* Every so often, map a process's worth of Areas into a fresh
         * domain and count the page-table pages it takes. */
        if (op % 250 == 249 && nheld) {
            struct arch_domain d;

            if (arch_domain_create(&d)) {
                unsigned n = nheld < SOAK_PROC ? nheld : SOAK_PROC;

                for (unsigned i = 0; i < n; i++)
                    area_map_domain(held[i], &d);
                r->tables += arch_domain_tables(&d);
                r->domains++;
                for (unsigned i = 0; i < n; i++)
                    area_unmap_domain(held[i], &d);
                arch_domain_destroy(&d);
            }
        }
    }
    r->largest_run = pmm_largest_free_run();
    while (nheld)
        area_destroy(held[--nheld]);
}

static void report(enum layout l, const struct soak_result *r)
{
    kprintf("V1.5 vq3 %s: %llu of %llu allocations failed, %llu with enough memory free; "
            "%llu ns each (zero-filling included); peak %llu MiB; largest free run after "
            "%llu MiB; %llu page-table pages per %u-Area process\n",
            layout_name(l), (unsigned long long)r->failures, (unsigned long long)r->attempts,
            (unsigned long long)r->needless,
            (unsigned long long)(time_to_us(r->alloc_ticks * 1000) / (r->attempts ? r->attempts : 1)),
            (unsigned long long)(r->peak_pages >> 8), (unsigned long long)(r->largest_run >> 8),
            (unsigned long long)(r->domains ? r->tables / r->domains : 0), SOAK_PROC);
}

void selftest_vq3(void)
{
    struct soak_result id, gl;
    uint64_t free_before = pmm_free_pages(), heap_before = kmalloc_pages();

    if (pmm_total() == 0 || !bootargs_has("vq3")) {
        kprintf("V1.5 vq3 skipped: %s\n", pmm_total() ? "not asked for (make vq3)" : "no managed memory");
        return;
    }
    soak(LAYOUT_IDENTITY, &id);
    soak(LAYOUT_GLOBAL, &gl);
    report(LAYOUT_IDENTITY, &id);
    report(LAYOUT_GLOBAL, &gl);
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before || area_count())
        fail("V1.5 vq3", "the soak leaked");
    if (gl.needless)
        fail("V1.5 vq3", "the global layout failed with memory free");
}

/* ---------------------------------------------------------------------
 * V2 step 1: ports and connections, run from the selftest task.
 */

static atomic_bool urgent_ran;
static spinlock_t test_lock;

static void urgent_waiter(void *)
{
    task_wait(SIGF_TEST);
    atomic_store(&urgent_ran, true);
}

/*
 * Waking a higher-priority task on this CPU while holding a lock must
 * not switch to it there and then, but must as soon as the lock goes:
 * by the IPI the CPU sends itself, with nothing else to interrupt it.
 */
static void test_wake_locked(void)
{
    unsigned me = ncpus > 1 ? 1 : 0;
    struct task *t;
    unsigned long was;
    uint64_t end;
    bool inside;

    task_set_affinity(cpu_mask(me));
    atomic_store(&urgent_ran, false);
    if (!(t = task_create("urgent", 20, cpu_mask(me), urgent_waiter, nullptr)))
        fail("V2.1 wake", "task_create failed");
    while (t->state != TASK_WAITING)    /* it outranks us: it ran at once */
        task_yield();
    was = spin_lock_irqsave(&test_lock);
    task_signal(t, SIGF_TEST);
    for (end = time_now() + time_us(200); time_now() < end;)
        arch_cpu_relax();
    inside = atomic_load(&urgent_ran);
    spin_unlock_irqrestore(&test_lock, was);
    for (end = time_now() + time_us(2000); !atomic_load(&urgent_ran) && time_now() < end;)
        arch_cpu_relax();
    if (inside)
        fail("V2.1 wake", "switched to the woken task with a lock held");
    if (!atomic_load(&urgent_ran))
        fail("V2.1 wake", "the woken task didn't run once the lock went");
    task_set_affinity(1);
    kprintf("V2.1 wake ok: a wakeup inside a lock switched only once it was let go\n");
}

/* Start two programs at once and wait (at most 'us') for both to die. */
static void run_pair(const char *a, uint64_t arg_a, const char *b, uint64_t arg_b, uint64_t us,
                     struct process *out[2])
{
    uint64_t start = time_now();

    task_set_signal(0, SIGF_DEAD);
    out[0] = process_create(a, 5, arg_a, task_self(), SIGF_DEAD);
    out[1] = process_create(b, 5, arg_b, task_self(), SIGF_DEAD);
    if (!out[0] || !out[1])
        fail("V2 run", "process_create failed");
    while ((!atomic_load(&out[0]->dead) || !atomic_load(&out[1]->dead)) &&
           time_now() - start < time_us(us))
        wait_test_dead();
}

static void expect_pair(const char *test, const char *a, uint64_t arg_a, const char *b,
                        uint64_t arg_b)
{
    struct process *p[2];

    run_pair(a, arg_a, b, arg_b, 5000000, p);
    for (int i = 0; i < 2; i++) {
        if (!atomic_load(&p[i]->dead))
            fail(test, "a process never ended");
        if (p[i]->fault != FAULT_NONE || p[i]->exit_code != 0) {
            kprintf("%s: %s exit %lld\n", test, p[i]->name, (long long)p[i]->exit_code);
            fail(test, "a process failed a check");
        }
        process_free(p[i]);
    }
}

void selftest_ipc(void)
{
    uint64_t free_before = pmm_free_pages(), heap_before = kmalloc_pages();
    unsigned handles_before = handle_count();

    test_wake_locked();
    if (!user_images[0].name) {
        kprintf("V2.1 conn skipped: no user programs\n");
        return;
    }
    expect_pair("V2.1 conn", "conn", 0, "conn", 1);
    kprintf("V2.1 conn ok: a port, a connection, its arena in both, signals both ways, "
            "the close heard\n");

    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
        fail("V2.1 reclaim", "pages leaked");
    if (handle_count() != handles_before || ipc_count() != 0 || area_count() != 0)
        fail("V2.1 reclaim", "handles, ports, connections or Areas leaked");
    kprintf("V2.1 reclaim ok\n");
}
