/*
 * conformance/callregs.c -- what a kernel call keeps, and what it clears
 *
 * 10,000 kernel calls, from three tasks at once while the kernel switches
 * between them and other programs: each must come back with the
 * callee-saved registers as they were, and nothing of the kernel's in
 * the scratch ones (code.h). Exits with the number of bad calls.
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "code.h"

static uint64_t bad[3];
static Handle main_task;

static void calls(uint64_t t)
{
    for (unsigned i = 0; i < 10000; i++)
        bad[t] += code_call_regs(SYS_SET_SIGNAL) != 0;
    if (t) {
        Signal(main_task, 1ULL << (20 + t));
        ExitTask(0);
    }
}

int main(void)
{
    uint64_t got = 0;

    main_task = FindTask(nullptr);
    CreateTask(calls, 1);
    CreateTask(calls, 2);
    calls(0);
    while (got != (3ULL << 21))
        got |= Wait(3ULL << 21);
    return (int)(bad[0] + bad[1] + bad[2]);
}
