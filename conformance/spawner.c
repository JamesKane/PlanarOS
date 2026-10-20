/*
 * conformance/spawner.c -- processes made by a process
 *
 * With arg 0: creates processes, held and not, and checks what
 * QueryProcess says about each way one ends (exit, fault, kill, never
 * started), and that bad arguments are refused. Exits 0 if every check
 * passed, otherwise with the number of the first that failed.
 *
 * With arg 1: starts two processes that spin forever and a held one,
 * and exits without closing their handles. The kernel then checks that
 * they died with it (V4 step 1).
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "log.h"

static unsigned step;

static int failed(const char *what)
{
    struct logbuf b = { 0 };

    log_str(&b, "spawner: check ");
    log_num(&b, step);
    log_str(&b, " failed: ");
    log_str(&b, what);
    KLog(b.text);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

/* Wait for 'h' to be dead and return its state. */
static uint32_t ended(int64_t h, int64_t sigbit, struct ProcessInfo *info)
{
    Wait(1ULL << sigbit);
    if (QueryProcess((Handle)h, info) != 0)
        return 0;
    return info->pi_State;
}

static int orphans(void)
{
    CreateProcess("spin", 0, 20, 0);
    CreateProcess("spin", 0, 20, 0);
    CreateProcess("hello", 0, 20, PROCF_HELD);
    return 0;
}

int main(uint64_t arg)
{
    struct ProcessInfo info;
    int64_t h;

    if (arg == 1)
        return orphans();

    /* Bad arguments. */
    CHECK(CreateProcess("nonesuch", 0, 20, 0) == ERR_NOTFOUND);
    CHECK(CreateProcess("hello", 0, 3, 0) == ERR_BADARG);          /* a system signal */
    CHECK(CreateProcess("hello", 0, 20, 1u << 7) == ERR_BADARG);
    CHECK(CreateProcess((const char *)8, 0, 20, 0) == ERR_BADADDR);
    CHECK(KillProcess(FindTask(nullptr)) == ERR_BADHANDLE);         /* a task, not a process */
    CHECK(CloseHandle(FindTask(nullptr)) == ERR_BADHANDLE);
    CHECK(QueryProcess(0, &info) == ERR_BADHANDLE);

    /* One that exits. */
    h = CreateProcess("hello", 0, 20, 0);
    CHECK(h > 0);
    CHECK(ended(h, 20, &info) == PROCS_EXITED && info.pi_ExitCode == 7);
    CHECK(CloseHandle((Handle)h) == 0);
    CHECK(QueryProcess((Handle)h, &info) == ERR_BADHANDLE);

    /* Held, then started once. */
    h = CreateProcess("hello", 0, 21, PROCF_HELD);
    CHECK(h > 0 && QueryProcess((Handle)h, &info) == 0 && info.pi_State == PROCS_HELD);
    CHECK(StartProcess((Handle)h) == 0);
    CHECK(StartProcess((Handle)h) == ERR_EXISTS);
    CHECK(ended(h, 21, &info) == PROCS_EXITED && info.pi_ExitCode == 7);
    CHECK(CloseHandle((Handle)h) == 0);

    /* Killed while it spins. */
    h = CreateProcess("spin", 0, 22, 0);
    CHECK(h > 0 && KillProcess((Handle)h) == 0);
    CHECK(ended(h, 22, &info) == PROCS_KILLED);
    CHECK(KillProcess((Handle)h) == 0);                         /* dead already: nothing */
    CHECK(CloseHandle((Handle)h) == 0);

    /* Faulted. */
    h = CreateProcess("rofault", 0, 23, 0);
    CHECK(h > 0 && ended(h, 23, &info) == PROCS_FAULTED && info.pi_Fault == PFAULT_WRITE);
    CHECK(CloseHandle((Handle)h) == 0);

    /* Held and never started: closing it ends it, and says so. */
    h = CreateProcess("hello", 0, 24, PROCF_HELD);
    CHECK(h > 0 && CloseHandle((Handle)h) == 0);
    Wait(1ULL << 24);

    /* Closed while it spins: killed (the kernel counts what's left). */
    h = CreateProcess("spin", 0, 25, 0);
    CHECK(h > 0 && CloseHandle((Handle)h) == 0);

    /* A child that leaves its own children behind. */
    h = CreateProcess("spawner", 1, 26, 0);
    CHECK(h > 0 && ended(h, 26, &info) == PROCS_EXITED && info.pi_ExitCode == 0);
    CHECK(CloseHandle((Handle)h) == 0);
    return 0;
}
