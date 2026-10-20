/*
 * kernel/selftest.c -- boot-time self-tests
 */
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "ktime.h"
#include "pmm.h"
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

static volatile int fired;

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
    while (!fired && time_now() - start < 50 * time_us(10000))
        arch_wait_for_interrupt();
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

static void start_cpu_timer(void *)
{
    unsigned i = this_cpu()->index;

    cpu_timers[i] = (struct ktimer){ .fn = cpu_timer_fn, .data = (void *)(uintptr_t)i };
    ktimer_start(&cpu_timers[i], time_now() + time_us(2000));
}

/* Each CPU's own timer: a 2 ms timer started on a CPU fires there. */
void selftest_cpu_timers(void)
{
    uint64_t start;

    if (ncpus == 1 || !time_have_timers()) {
        kprintf("V1.2 cpu timers skipped: %s\n", ncpus == 1 ? "one CPU" : "no timers");
        return;
    }
    for (unsigned i = 0; i < ncpus; i++) {
        struct cpu_call call = { .fn = start_cpu_timer };

        atomic_store(&fired_on[i], 0);
        cpu_call(&cpus[i], &call, true);
    }
    start = time_now();
    for (unsigned i = 0; i < ncpus; i++) {
        while (!atomic_load(&fired_on[i]) && time_now() - start < time_us(500000))
            arch_wait_for_interrupt();
        if (!atomic_load(&fired_on[i]))
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
