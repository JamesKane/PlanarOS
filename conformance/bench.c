/*
 * conformance/bench.c -- what the pieces of a round trip cost
 *
 * A kernel call that does nothing much (SetSignal), and Signal to the
 * calling task itself: a trap, a handle lookup, and the signal path
 * without a wakeup. Logs cycles per call.
 */
#include "cycles.h"
#include "log.h"

constexpr unsigned CALLS = 10000;

static void report(const char *what, const struct CycleSum *c)
{
    struct logbuf b = { 0 };

    log_str(&b, "bench: ");
    log_str(&b, what);
    log_str(&b, ": ");
    log_num(&b, CycleMean(c));
    log_str(&b, " cycles (");
    log_num(&b, c->dropped);
    log_str(&b, " moved CPU)");
    KLog(b.text);
}

int main(void)
{
    struct CycleSum null = { 0 }, self = { 0 }, read = { 0 };
    Handle me = FindTask(nullptr);
    uint64_t c0;

    for (unsigned i = 0; i < CALLS; i++) {
        c0 = ReadCycles();
        CycleAdd(&read, c0, ReadCycles());
    }
    for (unsigned i = 0; i < CALLS; i++) {
        c0 = ReadCycles();
        SetSignal(0, 0);
        CycleAdd(&null, c0, ReadCycles());
    }
    for (unsigned i = 0; i < CALLS; i++) {
        c0 = ReadCycles();
        Signal(me, 1ULL << 30);
        CycleAdd(&self, c0, ReadCycles());
    }
    report("reading the counter", &read);
    report("SetSignal(0, 0)", &null);
    report("Signal(self)", &self);
    return 0;
}
