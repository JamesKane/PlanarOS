/*
 * conformance/portwatch.c -- WatchPort (host-channel plan step 5)
 *
 * A watch on a name nobody has, fired when another process makes the
 * port (not by polling: no signal before); on a port already there,
 * fired at once; closed before it fires, never fired; three tasks
 * watching one name, each fired; the 17th of a process refused
 * (ERR_LIMIT); a bad signal or name refused, and a watch closed twice.
 * Last, a process killed holding watches: its handles back where they
 * were (the kernel's own count is the selftest's). Exits 0, or 1 with
 * the line.
 *
 * Argument 1 + (k << 8): make the port "pw.<'a' + k>" after 50 ms, then
 * wait to be killed. Argument 2: make 10 watches, then wait to be killed.
 */
#include <planar/ipc.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr int64_t SIG_A = 16, SIG_B = 17, SIG_T = 18, SIG_DEAD = 21, SIG_NAP = 22;
constexpr int64_t SIG_DONE0 = 24;       /* the three tasks', to the main one: 24 to 26 */
constexpr uint64_t MS = 1000000;
constexpr unsigned WATCHES = 16;        /* the kernel's PROCESS_WATCHES */

#define CHECK(c) \
    do { \
        if (!(c)) { \
            KPrintF("portwatch: check at line %d failed", __LINE__); \
            return 1; \
        } \
    } while (0)

static Handle main_task;

static void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_NAP);
    Wait(1ULL << SIG_NAP);
}

/* Argument 1: the port, made 50 ms in. */
static int maker(unsigned k)
{
    char name[] = "pw.a";

    name[3] = (char)('a' + k);
    nap(50);
    if (CreatePort(name, SIG_A) < 0)
        return 1;
    for (;;)
        nap(1000000);
}

/* Argument 2: watches held, until killed. */
static int holder(void)
{
    for (unsigned i = 0; i < 10; i++)
        if (WatchPort("pw.never", SIG_A) < 0)
            return 1;
    for (;;)
        nap(1000000);
}

/* A task's own watch on "pw.c": the main task told when it fired. */
static void watcher(uint64_t i)
{
    int64_t w = WatchPort("pw.c", SIG_T);

    if (w > 0 && WaitUntil(1ULL << SIG_T, CurrentTime() + 5000 * MS))
        Signal(main_task, 1ULL << (SIG_DONE0 + i));
    if (w > 0)
        CloseHandle((Handle)w);
    ExitTask(0);
}

/* A maker for port k, started. */
static int64_t make_port(unsigned k)
{
    return CreateProcess("portwatch", 1 + ((uint64_t)k << 8), SIG_DEAD, 0);
}

static void stop(int64_t p)
{
    KillProcess((Handle)p);
    CloseHandle((Handle)p);
}

int main(uint64_t arg)
{
    struct ProcessInfo pi;
    int64_t w, p, many[WATCHES + 1];
    uint64_t t, all, got = 0;
    uint32_t handles;

    if ((arg & 0xff) == 1)
        return maker((unsigned)(arg >> 8));
    if (arg == 2)
        return holder();
    main_task = FindTask(nullptr);

    /* A name nobody has: nothing till it's made, then the signal. */
    SetSignal(0, 1ULL << SIG_A);
    CHECK((w = WatchPort("pw.a", SIG_A)) > 0);
    CHECK(WaitUntil(1ULL << SIG_A, 0) == 0);
    t = CurrentTime();
    CHECK((p = make_port(0)) > 0);
    CHECK(WaitUntil(1ULL << SIG_A, t + 5000 * MS) == 1ULL << SIG_A);
    CHECK(CurrentTime() - t >= 40 * MS);
    CHECK(CloseHandle((Handle)w) == 0);
    CHECK(CloseHandle((Handle)w) == ERR_BADHANDLE);

    /* A port there already: at once. */
    CHECK((w = WatchPort("pw.a", SIG_A)) > 0);
    CHECK(WaitUntil(1ULL << SIG_A, 0) == 1ULL << SIG_A);
    CHECK(CloseHandle((Handle)w) == 0);
    stop(p);

    /* Closed before it fires: never. */
    SetSignal(0, 1ULL << SIG_B);
    CHECK((w = WatchPort("pw.b", SIG_B)) > 0);
    CHECK(CloseHandle((Handle)w) == 0);
    CHECK((p = make_port(1)) > 0);
    nap(200);
    CHECK(WaitUntil(1ULL << SIG_B, 0) == 0);
    stop(p);

    /* Three tasks watching one name: each fired. */
    all = 7ULL << SIG_DONE0;
    SetSignal(0, all);
    for (uint64_t i = 0; i < 3; i++)
        CHECK(CreateTask(watcher, i) > 0);
    nap(20);                            /* each has its watch */
    CHECK((p = make_port(2)) > 0);
    for (t = CurrentTime() + 5000 * MS; got != all;) {
        uint64_t s = WaitUntil(all, t);

        CHECK(s != 0);
        got |= s;
    }
    stop(p);

    /* A process's 16, and the 17th refused; refusals. */
    for (unsigned i = 0; i < WATCHES; i++)
        CHECK((many[i] = WatchPort("pw.never", SIG_A)) > 0);
    CHECK(WatchPort("pw.never", SIG_A) == ERR_LIMIT);
    for (unsigned i = 0; i < WATCHES; i++)
        CHECK(CloseHandle((Handle)many[i]) == 0);
    CHECK((w = WatchPort("pw.never", SIG_A)) > 0 && CloseHandle((Handle)w) == 0);
    CHECK(WatchPort("pw.never", 64) == ERR_BADARG);
    CHECK(WatchPort((const char *)8, SIG_A) == ERR_BADADDR);

    /* A process killed with watches out: its handles go with it. */
    CHECK(QueryProcess(0, &pi) == 0);
    handles = pi.pi_Handles;
    CHECK((p = CreateProcess("portwatch", 2, SIG_DEAD, 0)) > 0);
    for (t = CurrentTime() + 5000 * MS; QueryProcess((Handle)p, &pi) == 0 && pi.pi_Handles < 10;)
        CHECK(CurrentTime() < t && (nap(5), true));
    KillProcess((Handle)p);
    for (t = CurrentTime() + 5000 * MS; QueryProcess((Handle)p, &pi) == 0 && pi.pi_State < PROCS_EXITED;)
        CHECK(CurrentTime() < t && (nap(5), true));
    CloseHandle((Handle)p);
    CHECK(QueryProcess(0, &pi) == 0 && pi.pi_Handles == handles);
    KPrintF("portwatch: watches fired by CreatePort and at once, cancelled, three on a name, 16 a "
            "process, and gone with a killed watcher");
    return 0;
}
