/*
 * kernel/selftest.c -- boot-time self-tests
 */
#include "arch.h"
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
