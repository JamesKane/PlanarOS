/*
 * conformance/pool.c -- exec.library's pools, AllocVec and RawDoFmt
 * (V6.2)
 *
 * A pool's puddles filled, its blocks freed and taken again without a
 * new puddle; blocks above its threshold, each an Area; a pool in a
 * fixed buffer, filled to the last block; AREAF_CLEAR; DeletePool with
 * blocks outstanding, which leaves the process's handles where they
 * were. AllocVec at every size to 300 bytes and some larger, its flags,
 * and two tasks sharing its pool at once. Then conformance/fmtcheck.h's
 * RawDoFmt cases, and what each costs. Exits 0, or the step that failed.
 */
#include <planar/exec.h>

#include "fmtcheck.h"
#include "log.h"

#define CHECK(step, cond) \
    if (!(cond)) \
        return (step)

constexpr uint64_t PUDDLE = 16 * 1024, THRESHOLD = 1024;
constexpr unsigned BLOCKS = 256;            /* of 1 to 1024 bytes: several puddles */
constexpr unsigned ROUNDS = 2000;           /* each task's, sharing AllocVec's pool */

static uint8_t *block[BLOCKS];
static uint8_t fixed[8192];
static uint8_t *fixed_block[sizeof fixed / 16];
static volatile int task_failed;
static Handle first;

constexpr uint64_t DONE = 1ULL << 20;

static uint32_t handles(void)
{
    struct ProcessInfo pi;

    return QueryProcess(0, &pi) == 0 ? pi.pi_Handles : ~0u;
}

static uint64_t size_of(unsigned i)
{
    return 1 + (i * 37) % THRESHOLD;
}

static void fill(uint8_t *p, uint64_t n, unsigned tag)
{
    for (uint64_t i = 0; i < n; i++)
        p[i] = (uint8_t)(tag + i);
}

static bool holds(const uint8_t *p, uint64_t n, unsigned tag)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i] != (uint8_t)(tag + i))
            return false;
    return true;
}

static int pools(unsigned *puddles)
{
    uint32_t h0 = handles(), h1, h2;
    struct Pool *p = CreatePool(0, PUDDLE, THRESHOLD);
    uint8_t *big[3];

    CHECK(10, p && handles() == h0 + 1);
    CHECK(11, !CreatePool(0, 4096, 8192) && !CreatePool(AREAF_EXEC, PUDDLE, THRESHOLD));
    for (unsigned i = 0; i < BLOCKS; i++) {
        CHECK(12, (block[i] = AllocPooled(p, size_of(i))) && !((uintptr_t)block[i] & 15));
        fill(block[i], size_of(i), i);
    }
    h1 = handles();
    *puddles = h1 - h0;
    CHECK(13, *puddles > 2);
    for (unsigned i = 0; i < BLOCKS; i++)
        CHECK(14, holds(block[i], size_of(i), i));

    /* Freed, and taken again: from the lists, with no new puddle. */
    for (unsigned i = 0; i < BLOCKS; i++)
        FreePooled(p, block[i], size_of(i));
    for (unsigned i = 0; i < BLOCKS; i++) {
        CHECK(15, (block[i] = AllocPooled(p, size_of(BLOCKS - 1 - i))));
        fill(block[i], size_of(BLOCKS - 1 - i), i);
    }
    CHECK(16, handles() == h1);
    for (unsigned i = 0; i < BLOCKS; i++)
        CHECK(17, holds(block[i], size_of(BLOCKS - 1 - i), i));

    /* Above the threshold: an Area each, freed by FreePooled. */
    for (unsigned i = 0; i < 3; i++) {
        CHECK(18, (big[i] = AllocPooled(p, THRESHOLD + 1 + i * 50000)));
        fill(big[i], THRESHOLD + 1 + i * 50000, i);
    }
    h2 = handles();
    CHECK(19, h2 == h1 + 3 || h2 == h1 + 4);   /* their records may take a puddle */
    FreePooled(p, big[1], THRESHOLD + 1 + 50000);
    CHECK(20, handles() == h2 - 1 && holds(big[0], THRESHOLD + 1, 0) &&
                  holds(big[2], THRESHOLD + 1 + 100000, 2));
    CHECK(21, !AllocPooled(p, 0));

    /* Everything outstanding goes with the pool. */
    DeletePool(p);
    CHECK(22, handles() == h0);

    /* AREAF_CLEAR: a block taken again is zero. */
    CHECK(23, (p = CreatePool(AREAF_CLEAR, PUDDLE, THRESHOLD)));
    CHECK(24, (block[0] = AllocPooled(p, 100)));
    SetMem(block[0], 0xa5, 100);
    FreePooled(p, block[0], 100);
    CHECK(25, (block[1] = AllocPooled(p, 97)) == block[0]);    /* the same class */
    for (unsigned i = 0; i < 100; i++)
        CHECK(26, block[1][i] == 0);
    DeletePool(p);
    CHECK(27, handles() == h0);
    return 0;
}

/* A pool in a buffer: filled to its last block, never an Area. */
static int fixed_pool(unsigned *count)
{
    uint32_t h0 = handles();
    struct Pool *p = CreatePoolMem(fixed + 3, sizeof fixed - 3);
    unsigned n = 0, again = 0;

    CHECK(30, p);
    while ((fixed_block[n] = AllocPooled(p, 16))) {
        CHECK(31, fixed_block[n] >= fixed && fixed_block[n] + 16 <= fixed + sizeof fixed);
        fill(fixed_block[n], 16, n);
        n++;
    }
    CHECK(32, n > 300 && handles() == h0 && !AllocPooled(p, sizeof fixed));
    for (unsigned i = 0; i < n; i++)
        CHECK(33, holds(fixed_block[i], 16, i));
    for (unsigned i = 0; i < n; i++)
        FreePooled(p, fixed_block[i], 16);
    while (again < n && AllocPooled(p, 16))
        again++;
    CHECK(34, again == n && !AllocPooled(p, 16));
    DeletePool(p);
    CHECK(35, handles() == h0);
    *count = n;
    return 0;
}

static int vecs(void)
{
    uint32_t h0;
    uint8_t *v, *w;

    for (uint64_t n = 1; n <= 300; n++) {
        CHECK(40, (v = AllocVec(n, 0)) && !((uintptr_t)v & 15));
        fill(v, n, (unsigned)n);
        CHECK(41, (w = AllocVec(n, AREAF_CLEAR)));
        for (uint64_t i = 0; i < n; i++)
            CHECK(42, w[i] == 0);
        CHECK(43, holds(v, n, (unsigned)n));
        SetMem(w, 0xff, n);
        FreeVec(v);
        FreeVec(w);
    }
    for (uint64_t n = 4000; n <= 200000; n *= 3) {
        CHECK(44, (v = AllocVec(n, AREAF_CLEAR)));
        for (uint64_t i = 0; i < n; i++)
            CHECK(45, v[i] == 0);
        fill(v, n, 1);
        FreeVec(v);
    }
    /* Placed: an Area of its own. */
    h0 = handles();
    CHECK(46, (v = AllocVec(5000, AREAF_DMA32)) && (uintptr_t)v + 5000 <= 1ULL << 32);
    CHECK(47, handles() == h0 + 1);
    fill(v, 5000, 3);
    FreeVec(v);
    CHECK(48, handles() == h0);
    CHECK(49, !AllocVec(64, AREAF_EXEC) && !AllocVec(0, 0) && !AllocVec(~0ULL - 4, 0));
    FreeVec(nullptr);
    return 0;
}

/* Each task's blocks, allocated and freed among the other's. */
static int churn(unsigned tag)
{
    uint8_t *mine[16] = { 0 };

    for (unsigned r = 0; r < ROUNDS; r++) {
        unsigned i = r % 16;
        uint64_t n = 1 + (r * 131 + tag * 7) % 600;

        if (mine[i]) {
            if (!holds(mine[i], 1 + ((r - 16) * 131 + tag * 7) % 600, tag + r - 16))
                return 1;
            FreeVec(mine[i]);
        }
        if (!(mine[i] = AllocVec(n, 0)))
            return 2;
        fill(mine[i], n, tag + r);
    }
    for (unsigned i = 0; i < 16; i++)
        FreeVec(mine[i]);
    return 0;
}

static void count_put(char c, void *data)
{
    (void)c;
    (*(unsigned *)data)++;
}

static uint64_t format(const char *fmt, ...)
{
    unsigned n = 0;
    va_list ap;

    va_start(ap, fmt);
    RawDoFmt(fmt, ap, count_put, &n);
    va_end(ap);
    return n;
}

/* What each costs, in nanoseconds: a block taken from its list and given
 * back, AllocVec's the same through its lock, and a short line formatted. */
static void costs(void)
{
    constexpr unsigned N = 10000;
    struct Pool *p = CreatePool(0, PUDDLE, THRESHOLD);
    uint64_t t0, pooled, vec, fmt;
    void *v;

    if (!p)
        return;
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++) {
        v = AllocPooled(p, 64);
        FreePooled(p, v, 64);
    }
    pooled = (CurrentTime() - t0) / N;
    DeletePool(p);
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++)
        FreeVec(AllocVec(64, 0));
    vec = (CurrentTime() - t0) / N;
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++)
        format("%s %d at %#lx", "block", (int)i, (unsigned long)t0);
    fmt = (CurrentTime() - t0) / N;
    KPrintF("pool: AllocPooled and FreePooled %lu ns, AllocVec and FreeVec %lu ns, RawDoFmt of a "
            "short line %lu ns", pooled, vec, fmt);
}

static void second(uint64_t tag)
{
    task_failed = churn((unsigned)tag);
    Signal(first, DONE);
}

int main(void)
{
    struct fmtcheck_fail ff;
    unsigned puddles = 0, count = 0, fmts = 0;
    int r;

    first = FindTask(nullptr);
    if ((r = pools(&puddles)) || (r = fixed_pool(&count)) || (r = vecs()))
        return r;
    CHECK(50, CreateTask(second, 100) > 0);
    r = churn(200);
    Wait(DONE);
    CHECK(51, !r && !task_failed);
    if (fmt_check(&ff, &fmts)) {
        KPrintF("pool: RawDoFmt at fmtcheck.h:%u wrote \"%s\", not \"%s\"", ff.line, ff.got,
                ff.want);
        return 60;
    }
    KPrintF("pool: %u blocks over %u puddles and taken again, %u in a fixed buffer, AllocVec "
            "shared by 2 tasks; RawDoFmt %u cases", BLOCKS, puddles, count, fmts);
    costs();
    return 0;
}
