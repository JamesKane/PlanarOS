/*
 * kernel/cpuclass.c -- core classes, and what each is worth
 *
 * A core class is the CPUs of one core type (arch_cpu_type()): on the
 * CIX, eight Cortex-A720s and four Cortex-A520s. Each class's capacity
 * is measured, not looked up. The same fixed piece of work is timed on
 * every CPU, and a class's capacity is its CPUs' median time against
 * the fastest class's, which gets 1024. The CIX does describe its
 * speeds, but only in AML (_CPC), which the kernel doesn't read
 * (docs/09, D1); and a measurement gives the clocks the firmware left
 * set, which are the ones the kernel runs at until a power server
 * changes them.
 *
 * One capacity per class, not per CPU. The CIX's A720s come in four
 * clock grades, but the scheduler needs to tell big from little, and
 * per-CPU numbers would make placement on a symmetric machine follow
 * timing noise. The spread within a class is reported.
 */
#include "arch.h"
#include "cpu.h"
#include "cpuclass.h"
#include "kprintf.h"
#include "ktime.h"

constexpr unsigned WORK_N = 128;
constexpr unsigned WORK_ROUNDS = 16;
constexpr unsigned WORK_RUNS = 5;

static struct cpu_class classes[MAX_CPUS];
static unsigned nclasses, nmixed;
static volatile uint64_t work_seed = 0x2545f4914f6cdd1dULL, work_sink;

/*
 * Integer work of the kinds a kernel and its servers do: a sort (loads,
 * stores and branches), a hash over the result (independent multiplies,
 * which a wide core runs side by side) and a walk of a linked cycle
 * (dependent loads). Every CPU does exactly the same work.
 */
static uint64_t work(uint64_t x)
{
    uint32_t a[WORK_N];
    uint8_t next[WORK_N];
    uint64_t h = 0;

    for (unsigned i = 0; i < WORK_N; i++)
        next[i] = (uint8_t)((i * 37 + 11) % WORK_N);    /* one cycle through all */
    for (unsigned r = 0; r < WORK_ROUNDS; r++) {
        uint64_t h0 = 0, h1 = 0, h2 = 0, h3 = 0;
        unsigned p = r;

        for (unsigned i = 0; i < WORK_N; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            a[i] = (uint32_t)x;
        }
        for (unsigned i = 1; i < WORK_N; i++) {
            uint32_t v = a[i];
            unsigned j = i;

            for (; j > 0 && a[j - 1] > v; j--)
                a[j] = a[j - 1];
            a[j] = v;
        }
        for (unsigned i = 0; i < WORK_N; i += 4) {
            h0 = (h0 + a[i]) * 0x9e3779b97f4a7c15ULL;
            h1 = (h1 + a[i + 1]) * 0xc2b2ae3d27d4eb4fULL;
            h2 = (h2 + a[i + 2]) * 0x165667b19e3779f9ULL;
            h3 = (h3 + a[i + 3]) * 0x27d4eb2f165667c5ULL;
        }
        for (unsigned i = 0; i < 4 * WORK_N; i++)
            p = next[p ^ (a[p] & 1)];
        h ^= h0 ^ h1 ^ h2 ^ h3 ^ p;
    }
    return h;
}

/* On the CPU measured, interrupts off: the best of a few runs, so the
 * first, with cold caches, doesn't count. */
static void measure(void *)
{
    struct cpu *c = this_cpu();
    uint64_t best = ~0ULL;

    for (unsigned r = 0; r < WORK_RUNS; r++) {
        uint64_t t0 = time_now(), t;

        work_sink = work(work_seed);
        t = time_now() - t0;
        if (t < best)
            best = t;
    }
    c->work_ticks = best ? best : 1;
}

static void sort_u64(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t x = v[i];
        unsigned j = i;

        for (; j > 0 && v[j - 1] > x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
}

static unsigned count_clusters(unsigned cls)
{
    uint32_t seen[MAX_CPUS];
    unsigned n = 0;

    for (unsigned i = 0; i < ncpus; i++) {
        bool dup = false;

        if (cpus[i].cls != cls)
            continue;
        if (cpus[i].cluster == CPU_NO_CLUSTER)
            return 0;
        for (unsigned k = 0; k < n; k++)
            dup |= seen[k] == cpus[i].cluster;
        if (!dup)
            seen[n++] = cpus[i].cluster;
    }
    return n;
}

void cpu_classify(void)
{
    uint64_t fastest = ~0ULL;

    nclasses = nmixed = 0;
    for (unsigned i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        struct cpu_call call = { .fn = measure };
        unsigned k;

        c->cls = CPU_NO_CLASS;
        if (!atomic_load(&c->online) || !cpu_call(c, &call, true))
            continue;
        for (k = 0; k < nclasses && classes[k].type != c->type; k++)
            ;
        if (k == nclasses)
            classes[nclasses++] = (struct cpu_class){
                .type = c->type, .name = arch_cpu_type_name(c->type),
                .min_ticks = ~0ULL,
            };
        c->cls = k;
        classes[k].ncpus++;
        if (c->work_ticks < classes[k].min_ticks)
            classes[k].min_ticks = c->work_ticks;
        if (c->work_ticks > classes[k].max_ticks)
            classes[k].max_ticks = c->work_ticks;
    }

    for (unsigned k = 0; k < nclasses; k++) {
        uint64_t t[MAX_CPUS];
        unsigned n = 0;

        for (unsigned i = 0; i < ncpus; i++)
            if (cpus[i].cls == k)
                t[n++] = cpus[i].work_ticks;
        sort_u64(t, n);
        classes[k].median_ticks = t[n / 2];
        classes[k].nclusters = count_clusters(k);
        if (classes[k].median_ticks < fastest)
            fastest = classes[k].median_ticks;
    }
    for (unsigned k = 0; k < nclasses; k++) {
        uint64_t cap = (1024 * fastest + classes[k].median_ticks / 2) / classes[k].median_ticks;

        classes[k].capacity = cap ? cap : 1;
    }
    for (unsigned i = 0; i < ncpus; i++)
        if (cpus[i].cls != CPU_NO_CLASS)
            cpus[i].capacity = classes[cpus[i].cls].capacity;

    /* The PPTT's clusters, each of one type. */
    for (unsigned i = 0; i < ncpus; i++) {
        bool first = true, mixed = false;

        if (cpus[i].cls == CPU_NO_CLASS || cpus[i].cluster == CPU_NO_CLUSTER)
            continue;
        for (unsigned j = 0; j < ncpus; j++) {
            if (cpus[j].cls == CPU_NO_CLASS || cpus[j].cluster != cpus[i].cluster)
                continue;
            if (j < i)
                first = false;
            mixed |= cpus[j].type != cpus[i].type;
        }
        nmixed += first && mixed;
    }
}

void cpu_class_report(void)
{
    kprintf("planar: %u core class%s:\n", nclasses, nclasses == 1 ? "" : "es");
    for (unsigned k = 0; k < nclasses; k++) {
        const struct cpu_class *c = &classes[k];

        if (c->name)
            kprintf("planar:   %s:", c->name);
        else
            kprintf("planar:   core type %llx:", (unsigned long long)c->type);
        kprintf(" %u CPU%s", c->ncpus, c->ncpus == 1 ? "" : "s");
        if (c->nclusters)
            kprintf(" in %u cluster%s", c->nclusters, c->nclusters == 1 ? "" : "s");
        kprintf(", capacity %llu; the workload in %llu ns (%llu to %llu)\n",
                (unsigned long long)c->capacity,
                (unsigned long long)time_to_us(c->median_ticks * 1000),
                (unsigned long long)time_to_us(c->min_ticks * 1000),
                (unsigned long long)time_to_us(c->max_ticks * 1000));
    }
    kprintf("planar: the workload by CPU, ns:");
    for (unsigned i = 0; i < ncpus; i++)
        if (cpus[i].cls != CPU_NO_CLASS)
            kprintf(" %llx:%llu", (unsigned long long)cpus[i].hwid,
                    (unsigned long long)time_to_us(cpus[i].work_ticks * 1000));
    kprintf("\n");
    if (nmixed)
        kprintf("planar: %u PPTT cluster%s hold%s more than one core type\n", nmixed,
                nmixed == 1 ? "" : "s", nmixed == 1 ? "s" : "");
}

const struct cpu_class *cpu_class(unsigned i)
{
    return i < nclasses ? &classes[i] : nullptr;
}

unsigned cpu_nclasses(void)
{
    return nclasses;
}

unsigned cpu_clusters_mixed(void)
{
    return nmixed;
}
