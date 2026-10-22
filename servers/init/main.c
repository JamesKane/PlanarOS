/*
 * servers/init/main.c -- init, the root process (V6 step 7, VQ29, §12.7)
 *
 * The kernel starts init, not hwdb, with the device root as its second
 * argument. init starts hwdb held, with its own argument, gives it the
 * root (GrantRoot) and starts it, then waits for it to end. hwdb
 * supervises everything below it; init supervises hwdb, the one way
 * decision 7 chose: there's no restarting it, since a new hwdb would
 * have to adopt a dead one's leases and units.
 *
 * - hwdb ending as asked (exit 0: the kill test's end, or the Shell's
 *   EndShell with HWDB_END) ends init with the same code, and the kernel
 *   does what it did when hwdb ended: checks nothing outlived it, or
 *   reboots with "reboot".
 * - Any other end is a death. init logs a meditation, how hwdb ended and
 *   where, and resets the board (ColdReboot: its watchdog or PMIC, else
 *   PSCI or SBI). In test mode it doesn't reset: it reports, and exits
 *   with hwdb's code, as the kernel did.
 *
 * The meditation ends with hwdb's last lines, read back from the kernel's
 * log (ReadLog, which only the root's holder may call).
 *
 * INIT_DEATH (the kernel's V6.7 test) starts hwdb with HWDB_FAULT, so it
 * says so and faults at once: init must see the death, log the
 * meditation with that line last, and exits 0 if it did.
 */
#include <planar/lease.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

/* The argument's bits: hwdb's (servers/hwdb/server.h), and init's. */
constexpr uint64_t HWDB_TEST = 1u << 0;
constexpr uint64_t HWDB_FAULT = 1u << 2;    /* hwdb faults at once: init's test */
constexpr uint64_t HWDB_BITS = 0xff;
constexpr uint64_t INIT_DEATH = 1u << 8;    /* the kernel's test of hwdb's death */

constexpr int64_t SIG_HWDB = 16;

static const char *const faults[] = {
    "none", "a read", "a write", "an instruction fetch", "an illegal instruction",
    "a misaligned access", "a breakpoint", "an exception",
};

static void say(const char *format, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, format);
    VSNPrintf(line, sizeof line, format, ap);
    va_end(ap);
    KLog(line);
}

/* hwdb's last lines in the kernel log, oldest first. Whether the last
 * was 'expect', for the test. */
static bool last_words(Handle root, const char *expect)
{
    constexpr unsigned LINES = 8;
    static char log[4096];
    const char *line[LINES];
    unsigned len[LINES], n = 0;
    int64_t got = ReadLog(root, log, sizeof log);

    for (int64_t i = 0, start = 0; got > 0 && i < got; i++) {
        if (log[i] != '\n')
            continue;
        if (i - start > 7 && StrSame((struct Str){ &log[start], 7 }, StrFromC("[hwdb] "))) {
            line[n % LINES] = &log[start + 7];
            len[n % LINES] = (unsigned)(i - start - 7 - (log[i - 1] == '\r'));
            n++;
        }
        start = i + 1;
    }
    if (!n)
        say("init:   | (nothing: ReadLog gave %ld)", got);
    for (unsigned k = n > LINES ? n - LINES : 0; k < n; k++)
        say("init:   | %.*s", (int)len[k % LINES], line[k % LINES]);
    return n && StrSame((struct Str){ line[(n - 1) % LINES], len[(n - 1) % LINES] },
                        StrFromC(expect));
}

/* How hwdb ended, the Amiga's way: the box no one wanted to see; then
 * the last it said. Whether that ended with 'expect'. */
static bool meditation(const struct ProcessInfo *pi, Handle root, const char *expect)
{
    say("init: GURU MEDITATION: hwdb is dead");
    if (pi->pi_State == PROCS_FAULTED)
        say("init:   %s fault at %#lx, pc %#lx", pi->pi_Fault < 8 ? faults[pi->pi_Fault] : "a",
            pi->pi_FaultAddress, pi->pi_FaultPC);
    else if (pi->pi_State == PROCS_EXITED)
        say("init:   it exited %ld", pi->pi_ExitCode);
    else
        say("init:   it was killed");
    say("init:   its last words:");
    return last_words(root, expect);
}

int main(uint64_t arg, uint64_t root)
{
    struct ProcessInfo pi = { 0 };
    bool test = arg & (HWDB_TEST | INIT_DEATH), heard;
    uint64_t hwdb_arg = (arg & HWDB_BITS) | (arg & INIT_DEATH ? HWDB_FAULT : 0);
    int64_t p;

    SetSignal(0, 1ULL << SIG_HWDB);
    if ((p = CreateProcess("hwdb", hwdb_arg, SIG_HWDB, PROCF_HELD)) < 0) {
        say("init: hwdb couldn't be made (%ld)", p);
        return 2;
    }
    if (GrantRoot((Handle)root, (Handle)p) != 0 || StartProcess((Handle)p) != 0) {
        say("init: hwdb couldn't be given the root and started");
        CloseHandle((Handle)p);
        return 3;
    }
    while (QueryProcess((Handle)p, &pi) == 0 && pi.pi_State < PROCS_EXITED)
        Wait(1ULL << SIG_HWDB);
    CloseHandle((Handle)p);

    if (pi.pi_State == PROCS_EXITED && pi.pi_ExitCode == 0 && !(arg & INIT_DEATH))
        return 0;                   /* ended as asked */
    heard = meditation(&pi, (Handle)root, "hwdb: faulting at once, as init's test asks");
    if (arg & INIT_DEATH) {
        bool seen = pi.pi_State == PROCS_FAULTED;

        say("init: test: the board would be reset now%s", !seen ? ", but hwdb didn't fault"
                                                          : !heard ? ", but hwdb's last words weren't read"
                                                                   : "");
        return seen && heard ? 0 : 1;
    }
    if (test)
        return pi.pi_State == PROCS_EXITED ? (int)pi.pi_ExitCode : 100 + (int)pi.pi_Fault;
    say("init: resetting the board");
    ColdReboot((Handle)root);
    say("init: the board can't be reset: waiting for its switch");
    for (;;)
        Wait(0);
}
