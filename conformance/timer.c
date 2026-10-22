/*
 * conformance/timer.c -- CurrentTime and ArmTimer
 *
 * Checks that time goes forward, that an alarm comes no sooner than its
 * deadline and not much later, that one in the past comes at once, that
 * arming again replaces the alarm and a deadline of 0 disarms it, and
 * that tasks may exit with an alarm about to fire. Exits 0 if every
 * check passed, otherwise with the number of the first that failed.
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr uint64_t MS = 1000000;
constexpr unsigned RACERS = 200;

static unsigned step;

static int failed(const char *what)
{
    KPrintF("timer: check %lu failed: %s", (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static bool got(int64_t bit)
{
    return SetSignal(0, 0) & (1ULL << bit);
}

static void clear(int64_t bit)
{
    SetSignal(0, 1ULL << bit);
}

/* Arm an alarm due now, and leave. */
static void racer(uint64_t)
{
    ArmTimer(CurrentTime(), 20);
    ExitTask(0);
}

int main(void)
{
    uint64_t t0 = CurrentTime(), t1, late = 0;

    CHECK(CurrentTime() >= t0);
    CHECK(ArmTimer(t0 + MS, 3) == ERR_BADARG);          /* a system signal */

    /* No sooner than its deadline, and well within a second after. */
    t0 = CurrentTime();
    CHECK(ArmTimer(t0 + 2 * MS, 20) == 0);
    Wait(1ULL << 20);
    t1 = CurrentTime();
    CHECK(t1 >= t0 + 2 * MS && t1 < t0 + 1000 * MS);
    late = t1 - t0 - 2 * MS;

    /* In the past: at once. */
    CHECK(ArmTimer(1, 21) == 0);
    Wait(1ULL << 21);

    /* Arming again replaces it. */
    clear(22);
    t0 = CurrentTime();
    CHECK(ArmTimer(t0 + 3 * MS, 22) == 0 && ArmTimer(t0 + 6 * MS, 23) == 0);
    Wait(1ULL << 23);
    CHECK(!got(22));

    /* 0 disarms it. */
    clear(24);
    t0 = CurrentTime();
    CHECK(ArmTimer(t0 + 2 * MS, 24) == 0 && ArmTimer(0, 0) == 0);
    CHECK(ArmTimer(t0 + 5 * MS, 25) == 0);
    Wait(1ULL << 25);
    CHECK(!got(24));

    /* Tasks leaving with an alarm firing on another CPU. */
    for (unsigned i = 0; i < RACERS; i++) {
        while (CreateTask(racer, i) == ERR_LIMIT) {
            ArmTimer(CurrentTime() + MS / 10, 26);
            Wait(1ULL << 26);
        }
    }
    ArmTimer(CurrentTime() + 20 * MS, 26);
    Wait(1ULL << 26);

    KPrintF("timer: a 2 ms alarm came %lu us late", (uint64_t)(late / 1000));
    return 0;
}
