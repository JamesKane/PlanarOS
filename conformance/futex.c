/*
 * conformance/futex.c -- WaitAddr and WakeAddr (v5-plan step 1)
 *
 * Run twice: arg 0 is the test; 1 its peer, which it starts itself.
 * Alone, it checks the refusals, a word that already moved, deadlines
 * (to come and passed), and that WakeAddr wakes as many as it's told
 * and says how many. Then, across two processes, a sequence number in
 * their connection's arena is passed back and forth ROUNDS times, each
 * side sleeping on it until the other moves it, so a wake often comes
 * between a waiter's look at the word and its sleep. Last, the peer
 * waits on a word nobody moves and is killed there, while a task of the
 * test waits, with no deadline, on a word in the arena that only the
 * peer would have moved: the peer's death must wake it (§12.5). Exits 0 if every
 * check passed, otherwise with the number of the first that failed.
 */
#include <stdatomic.h>

#include <planar/ipc.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr unsigned ROUNDS = 2000;
constexpr unsigned SLEEPERS = 3;
constexpr uint64_t MS = 1000000;

static unsigned step;
static const char *role = "futex";

static int failed(const char *what)
{
    KPrintF("%s: check %lu failed: %s", role, (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static void sleep_ms(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, 30);
    Wait(1ULL << 30);
}

static volatile uint64_t gate;      /* the sleepers wait on it; nobody moves it */
static _Atomic unsigned woke;
static _Atomic unsigned orphan;     /* 1 waiting on the peer's word, 2 woken */

static void sleeper(uint64_t)
{
    if (WaitAddr(&gate, 0, 0) == 0)
        atomic_fetch_add(&woke, 1);
    ExitTask(0);
}

static void orphaned(uint64_t word)
{
    atomic_store(&orphan, 1);
    if (WaitAddr((volatile uint64_t *)word, 0, 0) == 0)
        atomic_store(&orphan, 2);
    ExitTask(0);
}

/* Wait until '*seq' is 'want', sleeping while it holds anything else. */
static bool await(volatile uint64_t *seq, uint64_t want)
{
    uint64_t v;

    while ((v = __atomic_load_n(seq, __ATOMIC_ACQUIRE)) != want)
        if (WaitAddr(seq, v, CurrentTime() + 2000 * MS) == ERR_TIMEOUT)
            return false;
    return true;
}

static void move(volatile uint64_t *seq, uint64_t to)
{
    __atomic_store_n(seq, to, __ATOMIC_RELEASE);
    WakeAddr(seq, 1);
}

static volatile uint64_t *seq_of(struct ConnArena *a)
{
    return (volatile uint64_t *)((char *)a + a->ca_Heap);
}

static int peer(void)
{
    int64_t sig = AllocSignal(-1), r;
    struct ConnArena *a = nullptr;
    volatile uint64_t *seq;

    role = "futex peer";
    CHECK(sig > 0);
    while ((r = OpenPort("futex", sig, 0, 0, &a)) == ERR_NOTFOUND)
        sleep_ms(1);
    CHECK(r > 0);
    seq = seq_of(a);
    for (uint64_t i = 0; i < ROUNDS; i++) {
        CHECK(await(seq, 2 * i + 1));
        move(seq, 2 * i + 2);
    }
    /* Now the test kills us, waiting with no deadline. */
    seq[1] = 1;
    WakeAddr(&seq[1], 1);
    WaitAddr(&seq[2], 0, 0);
    return failed("woken from a word nobody moves");
}

static int test(void)
{
    int64_t psig = AllocSignal(-1), csig = AllocSignal(-1), dsig = AllocSignal(-1), r, p;
    volatile uint64_t word = 5, *seq;
    struct ConnArena *a = nullptr;
    struct ProcessInfo pi;
    uint64_t t0, t;
    Handle port;

    CHECK(psig > 0 && csig > 0 && dsig > 0);

    /* Refusals: unaligned, unmapped. */
    CHECK(WaitAddr((volatile uint64_t *)((char *)&word + 4), 0, 0) == ERR_BADADDR);
    CHECK(WaitAddr((volatile uint64_t *)8, 0, 0) == ERR_BADADDR);
    CHECK(WakeAddr((volatile uint64_t *)8, 1) == ERR_BADADDR);
    CHECK(WakeAddr((volatile uint64_t *)((char *)&word + 1), 1) == ERR_BADADDR);

    /* A word that holds something else: back at once. Nobody to wake. */
    CHECK(WaitAddr(&word, 4, 0) == 0);
    CHECK(WakeAddr(&word, 1) == 0);

    /* Deadlines: one to come, kept to; one passed, at once. */
    t0 = CurrentTime();
    CHECK(WaitAddr(&word, 5, t0 + 20 * MS) == ERR_TIMEOUT);
    t = CurrentTime() - t0;
    CHECK(t >= 20 * MS && t < 1000 * MS);
    t0 = CurrentTime();
    CHECK(WaitAddr(&word, 5, 1) == ERR_TIMEOUT);
    CHECK(CurrentTime() - t0 < 100 * MS);
    CHECK(WaitAddr(&word, 4, 1) == 0);         /* the word first, then the deadline */

    /* Wake counts: SLEEPERS tasks on one word, two woken, then the rest. */
    for (unsigned i = 0; i < SLEEPERS; i++)
        CHECK(CreateTask(sleeper, 0) > 0);
    sleep_ms(50);
    CHECK(WakeAddr(&gate, 2) == 2);
    sleep_ms(50);
    CHECK(atomic_load(&woke) == 2);
    CHECK(WakeAddr(&gate, ~0u) == SLEEPERS - 2);
    sleep_ms(50);
    CHECK(atomic_load(&woke) == SLEEPERS);
    CHECK(WakeAddr(&gate, ~0u) == 0);

    /* Across processes, on a word in the connection's arena. */
    r = CreatePort("futex", psig);
    CHECK(r > 0);
    port = (Handle)r;
    p = CreateProcess("futex", 1, dsig, 0);
    CHECK(p > 0);
    while ((r = AcceptConn(port, csig, &a)) == ERR_NOTFOUND)
        Wait(1ULL << psig);
    CHECK(r > 0);
    seq = seq_of(a);
    t0 = CurrentTime();
    for (uint64_t i = 0; i < ROUNDS; i++) {
        move(seq, 2 * i + 1);
        CHECK(await(seq, 2 * i + 2));
    }
    t = CurrentTime() - t0;

    /* Killed while it waits: it goes, and leaves no waiter behind (the
     * kernel's side of the test counts them). */
    CHECK(await(&seq[1], 1));
    CHECK(CreateTask(orphaned, (uint64_t)&seq[3]) > 0);
    sleep_ms(20);
    CHECK(atomic_load(&orphan) == 1);
    CHECK(KillProcess((Handle)p) == 0);
    Wait(1ULL << dsig);
    CHECK(QueryProcess((Handle)p, &pi) == 0 && pi.pi_State == PROCS_KILLED);
    for (unsigned i = 0; i < 1000 && atomic_load(&orphan) != 2; i++)
        sleep_ms(1);
    CHECK(atomic_load(&orphan) == 2);
    CHECK(CloseHandle((Handle)p) == 0);
    CHECK(CloseConn((Handle)r) == 0);
    CHECK(DeletePort(port) == 0);

    KPrintF("futex: %lu round trips across two processes, %lu us each", (uint64_t)ROUNDS,
            (uint64_t)(t / ROUNDS / 1000));
    return 0;
}

int main(uint64_t arg)
{
    return arg ? peer() : test();
}
