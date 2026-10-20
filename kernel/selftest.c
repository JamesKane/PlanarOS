/*
 * kernel/selftest.c -- boot-time self-tests
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "acpi.h"
#include "arch.h"
#include "cpu.h"
#include "fdt.h"
#include "hw.h"
#include "kprintf.h"
#include "memmap.h"
#include "ktime.h"
#include "plat.h"
#include "pmm.h"
#include "area.h"
#include "bootargs.h"
#include "bootinfo.h"
#include "efi.h"
#include "handle.h"
#include "kmalloc.h"
#include "kstring.h"
#include "layout.h"
#include "port.h"
#include "process.h"
#include "prof.h"
#include "syscall.h"
#include "userimg.h"
#include "va.h"
#include "sched.h"
#include "selftest.h"

/* Harmless anywhere: kexec calls it from user mode, which must fault. */
[[gnu::noinline]] static uint64_t kernel_forty_two(void)
{
    return 42;
}

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
    /* Placing the bitmap: past a range it must not cover (review M15). */
    {
        uint64_t ks, ke, at, start;

        arch_kernel_extent(&ks, &ke);
        start = (ke + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        at = pmm_place(start, 3 * PAGE_SIZE, start + PAGE_SIZE, 2 * PAGE_SIZE);
        if (at < start + 3 * PAGE_SIZE)
            fail("V1.1 pmm", "placed the bitmap over a range it had to keep clear of");
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

/* --- the device-tree parser against blobs built to break it --- */

static uint32_t bad_blob[64];

static void be32_at(unsigned word, uint32_t v)
{
    uint8_t *p = (uint8_t *)&bad_blob[word];

    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A blob: the header, an empty reservation map at 40, the structure
 * block at 56 from 'words', and a strings block of "x" after it. */
static void build_blob(const uint32_t *words, unsigned n)
{
    unsigned so = 56, to = so + 4 * n;

    for (unsigned i = 0; i < 64; i++)
        bad_blob[i] = 0;
    be32_at(0, 0xd00dfeed);
    be32_at(1, to + 4);             /* totalsize */
    be32_at(2, so);
    be32_at(3, to);
    be32_at(4, 40);                 /* the reservation map */
    be32_at(5, 17);
    be32_at(6, 16);
    be32_at(8, 2);                  /* strings size */
    be32_at(9, 4 * n);              /* structure size */
    for (unsigned i = 0; i < n; i++)
        be32_at(so / 4 + i, words[i]);
    be32_at(to / 4, 0x78000000);    /* "x" */
}

/*
 * A property length that wraps the offset round to itself, more ends of
 * nodes than beginnings, blocks past the end of the blob, a node name
 * that never ends: the parser must give up on each, not hang or write
 * below its stack (review M14). The real tree, if any, is put back after.
 */
void selftest_fdt(void)
{
    const void *real = fdt_blob();
    static const uint32_t wraps[] = { 1, 0, 3, 0xfffffff4, 0, 2, 9 };
    static const uint32_t extra_ends[] = { 1, 0, 2, 2, 2, 1, 0x78000000, 2, 9 };
    static const uint32_t no_name_end[] = { 1, 0x61616161, 0x61616161, 0x61616161 };
    bool ok;

    build_blob(wraps, 7);
    ok = fdt_init(bad_blob) && fdt_find_name(-1, "x") < 0 &&
         fdt_getprop(0, "x", nullptr) == nullptr;
    build_blob(extra_ends, 9);
    ok &= fdt_init(bad_blob) && fdt_find_name(-1, "x") < 0;
    build_blob(no_name_end, 4);
    ok &= fdt_init(bad_blob) && fdt_find_name(-1, "aaa") < 0 && fdt_node_name(0)[0] == '\0';
    build_blob(wraps, 7);
    be32_at(2, 4096);               /* the structure block past the end */
    ok &= !fdt_init(bad_blob);
    if (real && !fdt_init(real))
        fail("V1.1 fdt", "the real device tree no longer reads");
    if (!real)
        fdt_init(nullptr);          /* booted from ACPI, or from nothing */
    if (!ok)
        fail("V1.1 fdt", "a broken device tree got past the parser");
    kprintf("V1.1 fdt ok: a wrapping length, extra node ends, blocks past the end and an "
            "endless name were all refused\n");
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

/* Signal bits don't count, so finishers count themselves, after
 * signalling: once the count is complete no signal is still on its way
 * to arrive after stop_spinners() has cleared it. */
static void spinner(void *arg)
{
    unsigned slot = (unsigned)(uintptr_t)arg;

    while (!atomic_load(&spin_stop))
        atomic_fetch_add(&spin_counts[slot], 1);
    task_signal(tester, SIGF_TEST);
    atomic_fetch_add(&spin_finished, 1);
}

static void stop_spinners(unsigned n)
{
    atomic_store(&spin_stop, true);
    while (atomic_load(&spin_finished) < n)
        task_sleep_until(time_now() + time_us(200));
    task_set_signal(0, SIGF_TEST);      /* every spinner's signal is in by now */
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

/* --- stranding: a task ready behind a busy CPU is taken by an idle one --- */

static atomic_bool strandee_ran;

static void strandee(void *)
{
    task_wait(SIGF_TEST);
    atomic_store(&strandee_ran, true);
}

/*
 * A low-priority task last ran on CPU 1, where a priority-20 hog now
 * runs; it may run on CPU 2 too, which sits idle. Woken, it must not
 * wait for the hog: CPU 2 has to be told (review H7). CPU 2 is idle
 * before the wakeup, so it only looks if someone interrupts it.
 */
static void test_strand(void)
{
    struct task *t;
    uint64_t end;

    if (ncpus < 3) {
        kprintf("V1.3 strand skipped: fewer than three CPUs\n");
        return;
    }
    atomic_store(&strandee_ran, false);
    if (!(t = task_create("strandee", 1, cpu_mask(1), strandee, nullptr)))
        fail("V1.3 strand", "task_create failed");
    while (t->state != TASK_WAITING || atomic_load(&t->on_cpu))
        task_sleep_until(time_now() + time_us(1000));
    t->affinity = cpu_mask(1) | cpu_mask(2);    /* waiting: safe to change */
    atomic_store(&spin_stop, false);
    atomic_store(&spin_finished, 0);
    atomic_store(&spin_counts[0], 0);
    if (!task_create("hog", 20, cpu_mask(1), spinner, (void *)0))
        fail("V1.3 strand", "task_create failed");
    while (atomic_load(&spin_counts[0]) == 0)
        task_sleep_until(time_now() + time_us(1000));
    task_signal(t, SIGF_TEST);
    for (end = time_now() + time_us(50000); !atomic_load(&strandee_ran) && time_now() < end;)
        task_sleep_until(time_now() + time_us(1000));
    if (!atomic_load(&strandee_ran))
        fail("V1.3 strand", "a ready task waited behind a busy CPU while another idled");
    stop_spinners(1);
    kprintf("V1.3 strand ok: a task ready behind a busy CPU ran on an idle one\n");
}

/* --- lowering: a running task lowered to a peer's level takes turns --- */

static atomic_bool lower_go;

static void lowered(void *)
{
    while (!atomic_load(&lower_go))
        arch_cpu_relax();
    task_set_pri(task_self(), 5);
    while (!atomic_load(&spin_stop))
        atomic_fetch_add(&spin_counts[0], 1);
    task_signal(tester, SIGF_TEST);
    atomic_fetch_add(&spin_finished, 1);
}

/* Lowered to the level of a ready peer, a spinning task must start
 * sharing its CPU (review M8). */
static void test_lower(void)
{
    unsigned k = ncpus > 1 ? 1 : 0;
    uint64_t end;

    atomic_store(&lower_go, false);
    atomic_store(&spin_stop, false);
    atomic_store(&spin_finished, 0);
    atomic_store(&spin_counts[0], 0);
    atomic_store(&spin_counts[1], 0);
    if (!task_create("lowered", 20, cpu_mask(k), lowered, nullptr) ||
        !task_create("peer", 5, cpu_mask(k), spinner, (void *)1))
        fail("V1.3 lower", "task_create failed");
    task_sleep_until(time_now() + time_us(2000));
    if (atomic_load(&spin_counts[1]))
        fail("V1.3 lower", "the peer ran under a higher priority");
    atomic_store(&lower_go, true);
    for (end = time_now() + time_us(50000); !atomic_load(&spin_counts[1]) && time_now() < end;)
        task_sleep_until(time_now() + time_us(1000));
    if (!atomic_load(&spin_counts[1]))
        fail("V1.3 lower", "a task lowered to a peer's priority never let it run");
    stop_spinners(2);
    kprintf("V1.3 lower ok: lowered to a ready peer's priority, a task took turns with it\n");
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


/* V3 step 1: is [base, base + size) reserved wherever it's RAM? */
static bool kept(uint64_t base, uint64_t size)
{
    uint64_t rb, rs;

    for (uint64_t a = base; a < base + size; ) {
        bool in_ram = false, moved = false;

        for (unsigned i = 0; memmap_reserved(i, &rb, &rs); i++)
            if (a >= rb && a < rb + rs) {
                a = rb + rs;
                moved = true;
                break;
            }
        if (moved)
            continue;
        for (unsigned i = 0; memmap_ram(i, &rb, &rs); i++)
            in_ram |= a >= rb && a < rb + rs;
        if (in_ram)
            return false;
        a += PAGE_SIZE;
    }
    return true;
}

/*
 * How the kernel was booted. Through UEFI: every range the EFI memory
 * map says the firmware or the loader keeps (its runtime services, ACPI,
 * the kernel, the map itself) is out of the page allocator's reach, and
 * so is the boot block.
 */
void selftest_boot(void)
{
    const struct boot_info *bi = bootinfo();
    unsigned descs = 0, kept_ranges = 0;

    if (!bi) {
        kprintf("V3.1 boot ok: from a device tree, no UEFI\n");
        return;
    }
    if (!kept(virt_to_phys(bi), sizeof *bi))
        fail("V3.1 boot", "the boot block is free memory");
    for (uint64_t off = 0; off + bi->mmap_desc_size <= bi->mmap_size; off += bi->mmap_desc_size) {
        const struct efi_memory_descriptor *d = phys_to_virt(bi->mmap + off);

        descs++;
        if (d->type == EFI_CONVENTIONAL_MEMORY || d->type == EFI_BOOT_SERVICES_CODE ||
            d->type == EFI_BOOT_SERVICES_DATA || !(d->attribute & EFI_MEMORY_WB))
            continue;
        kept_ranges++;
        if (!kept(d->physical_start, d->pages * EFI_PAGE_SIZE)) {
            kprintf("V3.1 boot: type %u at %p, %llu pages\n", d->type,
                    (void *)d->physical_start, (unsigned long long)d->pages);
            fail("V3.1 boot", "a range the firmware keeps is free memory");
        }
    }
    kprintf("V3.1 boot ok: through UEFI; %u memory descriptors, the %u the firmware or "
            "loader keeps all reserved; arguments \"%s\"\n", descs, kept_ranges, bi->cmdline);
}

/* ---------------------------------------------------------------------
 * V3 step 2: the ACPI parser against tables built here, good and broken.
 */

/* The tables: an RSDP, an XSDT and what it lists, at these offsets. */
enum { T_RSDP = 0, T_XSDT = 64, T_MADT = 256, T_GTDT = 768, T_SPCR = 1024, T_FACP = 1152,
       T_SIZE = 2048 };

/* Ways to break them. */
enum acpi_break {
    B_NONE, B_RSDP_SUM, B_RSDP_REV, B_TABLE_SUM, B_FAR, B_XSDT_TAIL, B_MADT_ZERO,
    B_MADT_PAST, B_GICC_SHORT, B_GTDT_COUNT, B_GTDT_SHORT, B_GTDT_PAST, B_LEN_HUGE,
    B_LEN_SHORT, B_COUNT,
};

static uint8_t acpi_buf[T_SIZE] __attribute__((aligned(16)));

static void put(uint32_t off, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
        acpi_buf[off + i] = (uint8_t)(v >> 8 * i);
}

static void put_sig(uint32_t off, const char *s, unsigned n)
{
    memcpy(&acpi_buf[off], s, n);
}

/* Byte 'at' set so the 'len' bytes from 'off' sum to zero. */
static void seal(uint32_t off, uint32_t len, uint32_t at)
{
    uint8_t sum = 0;

    acpi_buf[at] = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += acpi_buf[off + i];
    acpi_buf[at] = (uint8_t)-sum;
}

static void header(uint32_t off, const char *sig, uint32_t len)
{
    put_sig(off, sig, 4);
    put(off + 4, len, 4);
    acpi_buf[off + 8] = 1;
    put_sig(off + 10, "PLANARSELFTEST", 14);
}

static uint64_t pa_of(uint32_t off)
{
    return virt_to_phys(&acpi_buf[off]);
}

/* A machine like QEMU's: a GICv3, two CPUs, the timer, a PL011, PSCI
 * over HVC and an SBSA watchdog; broken as 'b' says. */
static void build_acpi(enum acpi_break b)
{
    uint32_t madt_len = 44 + 24 + 16 + 80 + 80, xsdt_len = 36 + 4 * 8, m, w, gtdt_len;

    memset(acpi_buf, 0, sizeof acpi_buf);

    m = T_MADT + 44;
    put(m, 0x0c | 24 << 8, 2);
    put(m + 8, 0x8000000, 8);
    acpi_buf[m + 20] = 3;
    m += 24;
    put(m, 0x0e | 16 << 8, 2);
    put(m + 4, 0x80a0000, 8);
    put(m + 12, 0x40000, 4);
    m += 16;
    for (unsigned cpu = 0; cpu < 2; cpu++, m += 80) {
        put(m, 0x0b | 80 << 8, 2);
        put(m + 8, cpu, 4);                 /* UID */
        put(m + 12, 1, 4);                  /* enabled */
        put(m + 68, cpu, 8);                /* MPIDR */
    }
    m -= 80;                                /* the second GICC */
    if (b == B_MADT_ZERO)
        acpi_buf[m + 1] = 0;
    if (b == B_MADT_PAST)
        acpi_buf[m + 1] = 120;
    if (b == B_GICC_SHORT) {
        acpi_buf[m + 1] = 40;
        madt_len -= 40;
    }
    header(T_MADT, "APIC", b == B_LEN_HUGE ? 0x7fffffff : madt_len);
    seal(T_MADT, madt_len, T_MADT + 9);

    /* The watchdog after the fixed part; or, broken, after an entry too
     * short to move past, or past the table's end. */
    w = b == B_GTDT_SHORT ? 107 : b == B_GTDT_PAST ? 140 : 104;
    gtdt_len = b == B_GTDT_SHORT ? 136 : 132;
    header(T_GTDT, "GTDT", gtdt_len);
    put(T_GTDT + 56, 30, 4);
    put(T_GTDT + 64, 27, 4);
    put(T_GTDT + 72, 26, 4);
    put(T_GTDT + 88, b == B_GTDT_COUNT ? 0xffffffff : b == B_GTDT_SHORT ? 2 : 1, 4);
    put(T_GTDT + 92, b == B_GTDT_PAST ? 140 : 104, 4);
    if (b == B_GTDT_SHORT)
        put(T_GTDT + 104, 0 | 3 << 8, 3);   /* three bytes long: no room for a body */
    put(T_GTDT + w, 1 | 28 << 8, 3);
    put(T_GTDT + w + 4, 0x2a450000, 8);
    put(T_GTDT + w + 12, 0x2a440000, 8);
    put(T_GTDT + w + 20, 59, 4);
    if (b == B_GTDT_COUNT)
        put(T_GTDT + w + 1, 0, 2);          /* and an endless run of empty ones */
    seal(T_GTDT, gtdt_len, T_GTDT + 9);
    if (b == B_TABLE_SUM)
        acpi_buf[T_GTDT + 100] ^= 1;

    header(T_SPCR, "SPCR", b == B_LEN_SHORT ? 8 : 80);
    acpi_buf[T_SPCR + 36] = 3;              /* PL011 */
    put(T_SPCR + 40, 0 | 32 << 8 | 3 << 24, 4);
    put(T_SPCR + 44, 0x9000000, 8);
    seal(T_SPCR, 80, T_SPCR + 9);

    header(T_FACP, "FACP", 140);
    put(T_FACP + 129, 3, 2);                /* PSCI, over HVC */
    seal(T_FACP, 140, T_FACP + 9);

    if (b == B_FAR)
        xsdt_len += 16;
    if (b == B_XSDT_TAIL)
        xsdt_len += 4;
    header(T_XSDT, "XSDT", xsdt_len);
    put(T_XSDT + 36, pa_of(T_MADT), 8);
    put(T_XSDT + 44, pa_of(T_GTDT), 8);
    put(T_XSDT + 52, pa_of(T_SPCR), 8);
    put(T_XSDT + 60, pa_of(T_FACP), 8);
    if (b == B_FAR) {
        put(T_XSDT + 68, ((uint64_t)ARCH_DIRECT_MAP_GIB << 30) - 8, 8);
        put(T_XSDT + 76, ~0ULL, 8);
    }
    if (b == B_XSDT_TAIL)
        put(T_XSDT + 68, 0xffffffff, 4);
    seal(T_XSDT, xsdt_len, T_XSDT + 9);

    put_sig(T_RSDP, "RSD PTR ", 8);
    acpi_buf[T_RSDP + 15] = b == B_RSDP_REV ? 0 : 2;
    put(T_RSDP + 20, 36, 4);
    put(T_RSDP + 24, pa_of(T_XSDT), 8);
    seal(T_RSDP, 20, T_RSDP + 8);
    seal(T_RSDP, 36, T_RSDP + 32);
    if (b == B_RSDP_SUM)
        acpi_buf[T_RSDP + 33] ^= 0x10;      /* reserved: only the extended checksum covers it */
}

/* What the parser should make of each: the RSDP refused, or what's
 * left of the machine. */
static bool acpi_case_ok(enum acpi_break b)
{
    struct acpi_gic g;
    struct acpi_cpu c;
    struct acpi_timer t;
    struct acpi_watchdog w;
    struct acpi_console con;
    unsigned found = 0;
    bool smc = true, ok;

    build_acpi(b);
    ok = acpi_init(acpi_buf);
    if (b == B_RSDP_SUM || b == B_RSDP_REV)
        return !ok && !acpi_table("APIC", nullptr);
    if (!ok)
        return false;
    for (unsigned i = 0; acpi_cpu(i, &c); i++)
        found += c.enabled && c.mpidr == i && c.uid == i;
    switch (b) {
    case B_MADT_ZERO:
    case B_MADT_PAST:
    case B_GICC_SHORT:
        return found == 1 && acpi_gic(&g);
    case B_LEN_HUGE:
        return !acpi_table("APIC", nullptr) && !acpi_cpu(0, &c) && acpi_timer(&t);
    case B_TABLE_SUM:
        return !acpi_table("GTDT", nullptr) && !acpi_timer(&t) && found == 2;
    case B_GTDT_COUNT:
    case B_GTDT_SHORT:
    case B_GTDT_PAST:
        return acpi_timer(&t) && !acpi_watchdog(0, &w);
    case B_LEN_SHORT:
        return !acpi_console(&con) && found == 2;
    default:
        break;
    }
    /* Whole, or broken only where nothing is read (B_FAR, B_XSDT_TAIL). */
    return found == 2 && acpi_gic(&g) && g.gicd == 0x8000000 && g.ngicr == 1 &&
           g.gicr[0].size == 0x40000 && g.version == 3 &&
           acpi_timer(&t) && t.el1_virt == 27 &&
           acpi_watchdog(0, &w) && w.refresh == 0x2a450000 && w.gsiv == 59 &&
           !acpi_watchdog(1, &w) &&
           acpi_console(&con) && con.kind == ACPI_UART_PL011 && con.base == 0x9000000 &&
           acpi_psci(&smc) && !smc;
}

/*
 * The choice of source (kernel/hw.c), then the parser: the good tables
 * read right, and each broken set is refused, or read only as far as it
 * is good, without a hang or a stray access. The real tables, if the
 * kernel is reading ACPI, are read again at the end.
 */
void selftest_acpi(void)
{
    const struct boot_info *bi = bootinfo();
    unsigned refused = 0;

    if (hw_choose(true, true, false) != HW_FDT || hw_choose(true, true, true) != HW_ACPI ||
        hw_choose(true, false, true) != HW_FDT || hw_choose(false, true, false) != HW_ACPI ||
        hw_choose(false, false, true) != HW_NONE)
        fail("V3.2 acpi", "the wrong firmware description was chosen");
    for (enum acpi_break b = B_NONE; b < B_COUNT; b++) {
        if (!acpi_case_ok(b)) {
            kprintf("V3.2 acpi: case %u\n", (unsigned)b);
            fail("V3.2 acpi", b ? "a broken table got past the parser"
                                : "good tables didn't read");
        }
        refused += b != B_NONE;
    }
    if (hw_source() == HW_ACPI) {
        if (!acpi_init(phys_to_virt(bi->rsdp)))
            fail("V3.2 acpi", "the real tables no longer read");
    } else {
        acpi_init(nullptr);
    }
    kprintf("V3.2 acpi ok: described by %s%s; good tables read, %u broken sets refused or "
            "cut short\n", hw_source_name(hw_source()),
            acpi_quirk_name() ? " (with quirks)" : "", refused);
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
    test_strand();
    test_lower();
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
    selftest_rings();
    kprintf("V2.2 done\n");
    selftest_domsg();
    kprintf("V2.3 done\n");
    selftest_death();
    kprintf("V2.4 done\n");
    selftest_boot();
    kprintf("V3.1 done\n");
    selftest_acpi();
    kprintf("V3.2 done\n");
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
    if (p->fault != FAULT_NONE || p->exit_code != code) {
        kprintf("%s: exit %lld, fault %d; wanted exit %lld\n", name, (long long)p->exit_code,
                p->fault, (long long)code);
        fail("V1.4 user", "wrong exit");
    }
    process_free(p);
}

static void expect_fault(const char *name, uint64_t arg, enum fault kind, uint64_t addr)
{
    struct process *p = run(name, arg, 1000000);

    if (!atomic_load(&p->dead))
        fail("V1.4 fault", "faulting process never ended");
    if (p->fault != kind) {
        kprintf("%s: fault %d, exit %lld; wanted fault %d\n", name, p->fault,
                (long long)p->exit_code, kind);
        fail("V1.4 fault", "wrong kind of fault");
    }
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
    expect_fault("ctrwrite", 0, FAULT_ILLEGAL, 0);
    expect_fault("kexec", (uint64_t)(uintptr_t)kernel_forty_two, FAULT_EXEC,
                 (uint64_t)(uintptr_t)kernel_forty_two);
    kprintf("V1.4 fault ok: wild write, write to code, read of the kernel, a call into "
            "the kernel, a write to the counters' controls each ended only their process\n");

    spin_us = expect_killed("spin");
    wait_us = expect_killed("waiter");
    kprintf("V1.4 kill ok: 3 spinning tasks dead %llu us after the kill, a blocked one %llu us\n",
            (unsigned long long)spin_us, (unsigned long long)wait_us);

    test_fpu();
    expect_exit("vecfp", 42);
    expect_exit("callregs", 0);

    /* Reclamation is asynchronous only by a switch; give it a moment. */
    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
        fail("V1.4 reclaim", "pages leaked");
    if (pmm_bad_frees())
        fail("V1.4 reclaim", "a page was freed that wasn\'t allocated");
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
    expect_fault("protrace", 0, FAULT_WRITE, 0);
    expect_exit("protflip", 0);
    kprintf("V1.5 rights ok: a write to a read-only Area faulted, and a write on another "
            "CPU just after ProtectArea took write away; a reader lived through 2000 rights changes\n");

    test_share();

    /* A global Area whose pages run out part way gives back what it
     * took, its page list included (review M5). The identity layout
     * takes one run, so fails before taking anything. */
    {
        uint64_t before = pmm_free_pages();
        struct area *a;

        pmm_fail_after(300);
        a = area_new(LAYOUT_GLOBAL, 600, 0, false);
        pmm_fail_after(0);
        if (a)
            fail("V1.5 nomem", "an Area was made with its pages failing");
        if (pmm_free_pages() != before)
            fail("V1.5 nomem", "a failed Area kept pages");
        kprintf("V1.5 nomem ok: an Area failing part way gave back every page\n");
    }

    expect_exit("arearace", 0);
    kprintf("V1.5 race ok: 3000 FreeAreas raced ShareArea and ProtectArea on the same handle; "
            "nothing mapped a freed Area\n");

    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
        fail("V1.5 reclaim", "pages leaked");
    if (pmm_bad_frees())
        fail("V1.5 reclaim", "a page was freed that wasn\'t allocated");
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
    /* A third process, connected to neither, for the rights checks. */
    task_set_signal(0, SIGF_DEAD);
    struct process *stranger = process_create("waiter", 5, 0, task_self(), SIGF_DEAD);

    if (!stranger)
        fail("V2.1 conn", "process_create failed");
    expect_pair("V2.1 conn", "conn", 0, "conn", stranger->tasks->handle);
    process_kill(stranger);
    while (!atomic_load(&stranger->dead))
        wait_test_dead();
    process_free(stranger);
    kprintf("V2.1 conn ok: a port, a connection, its arena in both, signals both ways, "
            "the close heard\n");

    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
        fail("V2.1 reclaim", "pages leaked");
    if (pmm_bad_frees())
        fail("V2.1 reclaim", "a page was freed that wasn\'t allocated");
    if (handle_count() != handles_before || ipc_count() != 0 || area_count() != 0)
        fail("V2.1 reclaim", "handles, ports, connections or Areas leaked");
    kprintf("V2.1 reclaim ok\n");
}

/* ---------------------------------------------------------------------
 * V2 step 2: rings, run from the selftest task.
 */

constexpr uint64_t STREAM = 20000;

/* For a hang: where each task of 'p' is. */
static void dump_tasks(struct process *p)
{
    for (struct task *t = p->tasks; t; t = t->next_in_proc)
        kprintf("  %s: state %d on_rq %d on_cpu %d cpu %u pri %d/%d received %llx waiting %llx\n",
                t->name, t->state, t->on_rq, atomic_load(&t->on_cpu), t->cpu->index, t->pri,
                t->base_pri, (unsigned long long)t->sig_received,
                (unsigned long long)t->sig_waiting);
    for (unsigned i = 0; i < ncpus; i++)
        kprintf("  cpu %u: current %s ready %u need_resched %d quantum %d\n", i,
                cpus[i].current->name, cpus[i].nr_ready, atomic_load(&cpus[i].need_resched),
                cpus[i].quantum_on);
}

/* Start the echo server and run 'client' with 'arg' against it. Returns
 * the kernel calls each made meanwhile: [0] the server, [1] the client. */
static void with_echo(const char *prog, uint64_t arg, uint64_t calls[2])
{
    struct process *echo, *client;
    uint64_t before;

    task_set_signal(0, SIGF_DEAD);
    if (!(echo = process_create("echo", 5, 0, task_self(), SIGF_DEAD)))
        fail("V2.2 echo", "process_create failed");
    task_sleep_until(time_now() + time_us(2000));       /* let it make its port */
    before = process_calls(echo);
    client = run(prog, arg, 10000000);
    calls[0] = process_calls(echo) - before;
    calls[1] = process_calls(client);
    if (!atomic_load(&client->dead)) {
        dump_tasks(client);
        dump_tasks(echo);
        fail("V2.2 echo", "the client never finished");
    }
    if (client->fault != FAULT_NONE || client->exit_code != 0) {
        kprintf("V2.2 echo: client exit %lld\n", (long long)client->exit_code);
        fail("V2.2 echo", "the client failed");
    }
    if (atomic_load(&echo->dead))
        fail("V2.2 echo", "the echo server died");
    process_free(client);
    process_kill(echo);
    while (!atomic_load(&echo->dead))
        wait_test_dead();
    process_free(echo);
}

void selftest_rings(void)
{
    uint64_t free_before = pmm_free_pages(), heap_before = kmalloc_pages(), calls[2];
    unsigned handles_before = handle_count();

    if (!user_images[0].name) {
        kprintf("V2.2 rings skipped: no user programs\n");
        return;
    }
    with_echo("client", 0, calls);
    kprintf("V2.2 echo ok: 2000 PutMsg/WaitPort and 2000 DoMsg round trips, every reply "
            "right\n");

    with_echo("client", STREAM, calls);
    /* Busy on both sides, a stream needs a kernel call only when one
     * side runs dry and sleeps: far fewer than one per message. */
    if (calls[0] + calls[1] > STREAM / 4)
        fail("V2.2 stream", "a kernel call for most messages");
    kprintf("V2.2 stream ok: %llu messages; kernel calls per 1000 messages: server %llu, "
            "client %llu\n", (unsigned long long)STREAM,
            (unsigned long long)(calls[0] * 1000 / STREAM),
            (unsigned long long)(calls[1] * 1000 / STREAM));

    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
        fail("V2.2 reclaim", "pages leaked");
    if (pmm_bad_frees())
        fail("V2.2 reclaim", "a page was freed that wasn\'t allocated");
    if (handle_count() != handles_before || ipc_count() != 0 || area_count() != 0)
        fail("V2.2 reclaim", "handles, ports, connections or Areas leaked");
    kprintf("V2.2 reclaim ok\n");
}

/* ---------------------------------------------------------------------
 * V2 step 3: DoMsg's handoff and priority loan, run from the selftest task.
 */

constexpr uint64_t SIGF_REPLY = 1ULL << 18;
static atomic_int server_cpu, server_pri;
static atomic_uint_fast64_t server_saw;     /* the bystander's count, when it ran */

/* Stands in for a server: each time it's woken it notes where and at
 * what priority it runs, and answers. */
static void fake_server(void *)
{
    for (;;) {
        task_wait(SIGF_TEST);
        atomic_store(&server_cpu, (int)this_cpu()->index);
        atomic_store(&server_pri, task_self()->pri);
        atomic_store(&server_saw, atomic_load(&spin_counts[1]));
        task_signal(tester, SIGF_REPLY);
    }
}

/* Wait for SIGF_REPLY for at most 'us'. */
static bool wait_reply(uint64_t us)
{
    struct task *self = task_self();
    uint64_t got;

    task_set_signal(0, SIGF_SLEEP);
    self->sleep = (struct ktimer){ .fn = timeout_fire, .data = self };
    ktimer_start(&self->sleep, time_now() + time_us(us));
    got = task_wait(SIGF_REPLY | SIGF_SLEEP);
    ktimer_stop(&self->sleep);
    return got & SIGF_REPLY;
}

/* Wake 't' with a handoff and wait for its answer, as SignalWaitConn does. */
static bool post_and_wait(struct task *t, uint64_t us)
{
    struct cpu *c = task_post(t, SIGF_TEST, POST_HANDOFF | POST_LEND);
    bool ok;

    if (c && c != this_cpu())
        sched_kick(c);
    ok = wait_reply(us);
    if (c == this_cpu())
        sched_kick(c);
    return ok;
}

/*
 * A handoff brings a server waiting on another CPU to this one. A loan
 * lets a low-priority server run past a medium-priority task hogging
 * the CPU, and ends when the server waits again.
 */
static void test_handoff(void)
{
    unsigned k = ncpus > 1 ? 1 : 0, other = ncpus > 2 ? 2 : k;
    struct task *server;

    task_set_affinity(cpu_mask(k));
    task_set_signal(0, SIGF_REPLY);
    if (!(server = task_create("server", 1, cpu_mask(other), fake_server, nullptr)))
        fail("V2.3 handoff", "task_create failed");
    while (server->state != TASK_WAITING || atomic_load(&server->on_cpu))
        task_sleep_until(time_now() + time_us(1000));
    server->affinity = cpu_mask(k) | cpu_mask(other);  /* waiting: safe to change */
    if (!post_and_wait(server, 100000) || atomic_load(&server_cpu) != (int)k)
        fail("V2.3 handoff", "the server didn't run on the waker's CPU");

    /* Now on this CPU only, under a medium-priority hog. */
    while (server->state != TASK_WAITING || atomic_load(&server->on_cpu))
        task_sleep_until(time_now() + time_us(1000));
    server->affinity = cpu_mask(k);
    atomic_store(&spin_stop, false);
    atomic_store(&spin_finished, 0);
    atomic_store(&spin_counts[0], 0);
    if (!task_create("hog", 5, cpu_mask(k), spinner, (void *)0))
        fail("V2.3 loan", "task_create failed");
    if (!post_and_wait(server, 200000))
        fail("V2.3 loan", "a low-priority server never ran under a medium-priority hog");
    if (atomic_load(&server_pri) != 10)
        fail("V2.3 loan", "the server didn't run at the waker's priority");
    while (server->state != TASK_WAITING || atomic_load(&server->on_cpu))
        task_sleep_until(time_now() + time_us(1000));
    if (server->pri != 1)
        fail("V2.3 loan", "the loan outlived the server's next wait");

    /* Ahead of a ready task of the same priority: the bystander mustn't
     * run between the handoff and the server. Interrupts stay off from
     * the snapshot to the wait, so the quantum can't come in between.
     * (After the server, the bystander does run before this task: the
     * server's answer queues this task behind it.) */
    atomic_store(&spin_counts[1], 0);
    if (!task_create("bystander", 10, cpu_mask(k), spinner, (void *)1))
        fail("V2.3 front", "task_create failed");
    while (atomic_load(&spin_counts[1]) == 0)
        task_yield();
    unsigned long was = arch_irq_save();
    uint64_t before = atomic_load(&spin_counts[1]);
    atomic_store(&server_pri, 0);
    task_post(server, SIGF_TEST, POST_HANDOFF | POST_LEND);
    task_wait(SIGF_REPLY);
    arch_irq_restore(was);
    bool first = atomic_load(&server_saw) == before;
    if (!first || atomic_load(&server_pri) != 10)
        fail("V2.3 front", "a ready task ran between the handoff and the server");
    stop_spinners(2);
    task_set_affinity(1);
    kprintf("V2.3 handoff ok: a waiting server moved to the waker's CPU; lent priority 10 "
            "it ran past a hog at 5 and ahead of a peer at 10, and went back to 1\n");
}

/* What the kernel's own building blocks cost on this CPU, to read the
 * round trip's cost against. */
static void pieces(void)
{
    constexpr unsigned N = 10000;
    static spinlock_t l;
    static atomic_uint_fast64_t a;
    _Atomic uint64_t n = 0;
    uint64_t c0, lock, amo, irq, store, yield;
    unsigned long was = arch_irq_save();

    c0 = arch_read_cycles();
    for (unsigned i = 0; i < N; i++) {
        unsigned long w = spin_lock_irqsave(&l);
        spin_unlock_irqrestore(&l, w);
    }
    lock = arch_read_cycles() - c0;
    c0 = arch_read_cycles();
    for (unsigned i = 0; i < N; i++)
        atomic_fetch_add(&a, 1);
    amo = arch_read_cycles() - c0;
    c0 = arch_read_cycles();
    for (unsigned i = 0; i < N; i++)
        arch_irq_restore(arch_irq_save());
    irq = arch_read_cycles() - c0;
    c0 = arch_read_cycles();
    for (unsigned i = 0; i < N; i++)
        cpu_count(&n);
    store = arch_read_cycles() - c0;
    struct arch_domain d;
    uint64_t dom = 0;

    if (arch_domain_create(&d)) {
        c0 = arch_read_cycles();
        for (unsigned i = 0; i < N; i++) {
            arch_domain_switch(&d);
            arch_domain_switch(nullptr);
        }
        dom = (arch_read_cycles() - c0) / 2;
        arch_domain_destroy(&d);
    }
    arch_irq_restore(was);
    c0 = arch_read_cycles();
    for (unsigned i = 0; i < N; i++)
        task_yield();       /* nothing else ready here: picks itself */
    yield = arch_read_cycles() - c0;
    kprintf("V2.3 pieces: cycles for a lock and unlock %llu, an atomic add %llu, "
            "interrupts off and on %llu, a per-CPU count %llu, schedule() to itself %llu, "
            "a domain switch %llu\n",
            (unsigned long long)(lock / N), (unsigned long long)(amo / N),
            (unsigned long long)(irq / N), (unsigned long long)(store / N),
            (unsigned long long)(yield / N), (unsigned long long)(dom / N));
}

void selftest_domsg(void)
{
    uint64_t calls[2], before, start, ns;

    test_handoff();
    if (!user_images[0].name) {
        kprintf("V2.3 domsg skipped: no user programs\n");
        return;
    }
    pieces();
    expect_exit("bench", 0);
    before = sched_handoffs();
    start = time_now();
    prof_reset();
    with_echo("pingpong", 0, calls);
    prof_report("pingpong, both CPUs' worth", 20000);
    ns = time_to_us((time_now() - start) * 1000) / 20000;
    if (sched_handoffs() - before < 20000 / 2)
        fail("V2.3 domsg", "DoMsg seldom handed the CPU to the server");
    kprintf("V2.3 domsg ok: %llu handoffs in 20000 round trips; kernel calls per round trip: "
            "server %llu.%02llu, client %llu.%02llu; about %llu ns each, start to end\n",
            (unsigned long long)(sched_handoffs() - before),
            (unsigned long long)(calls[0] / 20000), (unsigned long long)(calls[0] % 20000 / 200),
            (unsigned long long)(calls[1] / 20000), (unsigned long long)(calls[1] % 20000 / 200),
            (unsigned long long)ns);
}

/* ---------------------------------------------------------------------
 * V2 step 4: death notices and load, run from the selftest task.
 */

static struct process *start(const char *test, const char *name, uint64_t arg)
{
    struct process *p = process_create(name, 5, arg, task_self(), SIGF_DEAD);

    if (!p)
        fail(test, "process_create failed");
    return p;
}

/* Wait (at most 'us') for 'p' to die. */
static bool wait_dead(struct process *p, uint64_t us)
{
    uint64_t begin = time_now();

    while (!atomic_load(&p->dead) && time_now() - begin < time_us(us))
        wait_test_dead();
    return atomic_load(&p->dead);
}

static void kill_and_free(struct process *p)
{
    process_kill(p);
    if (!wait_dead(p, 1000000))
        fail("V2.4", "a killed process never ended");
    process_free(p);
}

static struct process *also_dump;   /* for a hang: another process to show */

static void expect_clean_exit(const char *test, struct process *p, uint64_t us)
{
    if (!wait_dead(p, us)) {
        dump_tasks(p);
        if (also_dump)
            dump_tasks(also_dump);
        fail(test, "a process never ended");
    }
    if (p->fault != FAULT_NONE || p->exit_code != 0) {
        kprintf("%s: %s exit %lld\n", test, p->name, (long long)p->exit_code);
        fail(test, "a process failed a check");
    }
    process_free(p);
}

/* Is the first task of 'p' blocked in a wait? */
static bool blocked(struct process *p)
{
    unsigned long was = spin_lock_irqsave(&p->lock);
    bool b = p->tasks && p->tasks->state == TASK_WAITING && !atomic_load(&p->tasks->on_cpu);

    spin_unlock_irqrestore(&p->lock, was);
    return b;
}

/* Signal the first task of 'p'. */
static void signal_first(struct process *p, uint64_t sig)
{
    unsigned long was = spin_lock_irqsave(&p->lock);

    if (p->tasks)
        task_signal(p->tasks, sig);
    spin_unlock_irqrestore(&p->lock, was);
}

/* A client blocked in DoMsg hears that its server died. */
static void test_server_dies(void)
{
    struct process *sink, *doomed;
    unsigned tries = 0;

    task_set_signal(0, SIGF_DEAD);
    sink = start("V2.4 server", "sink", 0);
    doomed = start("V2.4 server", "doomed", 0);
    /* Both asleep for a while: the message sits with the server. */
    do {
        task_sleep_until(time_now() + time_us(5000));
        if (++tries > 400)
            fail("V2.4 server", "client and server never settled");
    } while (!blocked(sink) || !blocked(doomed));
    kill_and_free(sink);
    expect_clean_exit("V2.4 server", doomed, 1000000);
    kprintf("V2.4 server ok: a client blocked in DoMsg got ERR_PORT_DIED when its server "
            "was killed\n");
}

/* A server drops a client that dies, or that breaks the rules, and goes
 * on serving others. */
static void test_client_dies(void)
{
    struct process *echo, *talker;
    uint64_t begin;

    task_set_signal(0, SIGF_DEAD);
    echo = start("V2.4 client", "echo", 0);
    talker = start("V2.4 client", "talker", 0);
    task_sleep_until(time_now() + time_us(20000));
    if (atomic_load(&talker->dead) || ipc_count() != 2)
        fail("V2.4 client", "the talker isn't talking");
    kill_and_free(talker);
    for (begin = time_now(); ipc_count() != 1 && time_now() - begin < time_hz();)
        task_sleep_until(time_now() + time_us(1000));
    if (ipc_count() != 1)
        fail("V2.4 client", "the server kept a dead client's connection");

    for (uint64_t how = 0; how < 4; how++)
        expect_clean_exit("V2.4 rogue", start("V2.4 rogue", "rogue", how), 1000000);
    if (atomic_load(&echo->dead))
        fail("V2.4 rogue", "a rogue client killed the server");
    expect_clean_exit("V2.4 rogue", start("V2.4 rogue", "client", 0), 10000000);
    expect_clean_exit("V2.4 limits", start("V2.4 limits", "limits", 0), 5000000);
    uint64_t drops = syscall_klog_drops();

    expect_clean_exit("V2.4 limits", start("V2.4 limits", "logflood", 0), 5000000);
    if (syscall_klog_drops() - drops < 400)
        fail("V2.4 limits", "a process flooded the console");
    kprintf("V2.4 limits ok: a process stopped at 64 tasks, 256 handles and 32 connections; "
            "a flood of KLog lines was cut to its rate, escapes filtered\n");
    kill_and_free(echo);
    kprintf("V2.4 client ok: the server dropped a killed client, and four rogue ones "
            "(a pointer outside the heap, a broken ring, scribbling, a message running off the heap), and served on\n");
}

/* omega's run under load: two conversations go on while competitors
 * hog the CPUs and other processes crash, and every reply is right. */
static void test_load(void)
{
    struct process *echo, *talkers[2], *hog;
    uint64_t end;
    unsigned crashes = 0;

    task_set_signal(0, SIGF_DEAD);
    echo = start("V2.4 load", "echo", 0);
    talkers[0] = start("V2.4 load", "talker", 0);
    talkers[1] = start("V2.4 load", "talker", 0);
    hog = start("V2.4 load", "spin", 0);
    for (end = time_now() + time_us(300000); time_now() < end; crashes++) {
        struct process *p = start("V2.4 load", crashes % 2 ? "wild" : "wx", 0);

        if (!wait_dead(p, 1000000) || p->fault != FAULT_WRITE)
            fail("V2.4 load", "a crash didn't end its process");
        process_free(p);
        task_sleep_until(time_now() + time_us(10000));
    }
    kill_and_free(hog);
    for (int i = 0; i < 2; i++)
        signal_first(talkers[i], SIGBREAKF_CTRL_C);
    also_dump = echo;
    for (int i = 0; i < 2; i++)
        expect_clean_exit("V2.4 load", talkers[i], 2000000);
    also_dump = nullptr;
    if (atomic_load(&echo->dead))
        fail("V2.4 load", "the server died");
    kill_and_free(echo);
    kprintf("V2.4 load ok: two conversations kept every reply right past a hog and %u "
            "crashes\n", crashes);
}

void selftest_death(void)
{
    uint64_t free_before = pmm_free_pages(), heap_before = kmalloc_pages();
    unsigned handles_before = handle_count();

    if (!user_images[0].name) {
        kprintf("V2.4 death skipped: no user programs\n");
        return;
    }
    test_server_dies();
    expect_pair("V2.4 stash", "stashserver", 0, "stasher", 0);
    kprintf("V2.4 stash ok: DoMsg set two replies aside and GetMsg gave them back in order, "
            "while the server rewrote their links\n");
    test_client_dies();
    test_load();

    task_sleep_until(time_now() + time_us(5000));
    if (pmm_free_pages() + (kmalloc_pages() - heap_before) != free_before)
        fail("V2.4 reclaim", "pages leaked");
    if (pmm_bad_frees())
        fail("V2.4 reclaim", "a page was freed that wasn\'t allocated");
    if (handle_count() != handles_before || ipc_count() != 0 || area_count() != 0 ||
        process_count() != 0)
        fail("V2.4 reclaim", "handles, ports, connections, Areas or processes leaked");
    kprintf("V2.4 reclaim ok\n");
}
