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
 * INIT_DEATH (the kernel's V6.7 test) starts hwdb with HWDB_FAULT, so it
 * faults at once: init must see the death and log the meditation, and
 * exits 0 if it did.
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

/* How hwdb ended, the Amiga's way: the box no one wanted to see. */
static void meditation(const struct ProcessInfo *pi)
{
    say("init: GURU MEDITATION: hwdb is dead");
    if (pi->pi_State == PROCS_FAULTED)
        say("init:   %s fault at %#lx, pc %#lx", pi->pi_Fault < 8 ? faults[pi->pi_Fault] : "a",
            pi->pi_FaultAddress, pi->pi_FaultPC);
    else if (pi->pi_State == PROCS_EXITED)
        say("init:   it exited %ld", pi->pi_ExitCode);
    else
        say("init:   it was killed");
}

int main(uint64_t arg, uint64_t root)
{
    struct ProcessInfo pi = { 0 };
    bool test = arg & (HWDB_TEST | INIT_DEATH);
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
    meditation(&pi);
    if (arg & INIT_DEATH) {
        bool seen = pi.pi_State == PROCS_FAULTED;

        say("init: test: the board would be reset now%s", seen ? "" : ", but hwdb didn't fault");
        return seen ? 0 : 1;
    }
    if (test)
        return pi.pi_State == PROCS_EXITED ? (int)pi.pi_ExitCode : 100 + (int)pi.pi_Fault;
    say("init: resetting the board");
    ColdReboot((Handle)root);
    say("init: the board can't be reset: waiting for its switch");
    for (;;)
        Wait(0);
}
