/*
 * conformance/tls.c -- thread_local: a copy for each task (V6.1)
 *
 * Two tasks of one process, the first and a CreateTask's, each start with
 * the variables' first values (.tdata) and zeros (.tbss), set their own,
 * and find them unchanged after the other has set its. Each task's copy
 * is in its own stack (lib/rt/tls.c). The second task ends by returning,
 * which crt0's trampoline turns into ExitTask. Exits 0, or the step that
 * failed.
 */
#include <planar/exec.h>

#include "log.h"

static thread_local uint64_t counter = 42;
static thread_local uint64_t zeroed[32];
static thread_local _Alignas(64) uint8_t line[64] = { 7 };

static Handle first;
static volatile int failed;
static struct AreaInfo where[2], stack[2];     /* each task's copy's Area, and a local's */

constexpr uint64_t SET = 1ULL << 20, CHECKED = 1ULL << 21, GONE = 1ULL << 22;

/* Fresh values, then this task's own: 0, or the step that failed. */
static int check_fresh(void)
{
    if (counter != 42 || line[0] != 7 || line[1] != 0 || ((uintptr_t)line & 63))
        return 1;
    for (unsigned i = 0; i < 32; i++)
        if (zeroed[i])
            return 2;
    return 0;
}

static void set(uint64_t v)
{
    counter = v;
    line[0] = (uint8_t)v;
    for (unsigned i = 0; i < 32; i++)
        zeroed[i] = v + i;
}

static bool still(uint64_t v)
{
    if (counter != v || line[0] != (uint8_t)v)
        return false;
    for (unsigned i = 0; i < 32; i++)
        if (zeroed[i] != v + i)
            return false;
    return true;
}

static void second(uint64_t arg)
{
    int r = check_fresh();
    volatile int local = 0;

    if (QueryArea(&counter, &where[1]) != 0 || QueryArea((const void *)&local, &stack[1]) != 0)
        r = 3;                      /* looked at now: the stack goes when the task does */
    if (r) {
        failed = 10 + r;
    } else {
        set(arg);
        Signal(first, SET);
        Wait(CHECKED);              /* the first task has set its own */
        if (!still(arg))
            failed = 13;
    }
    Signal(first, GONE);
}                                   /* returns: the trampoline's ExitTask */

int main(void)
{
    volatile int local = 0;
    int64_t h;
    int r;

    first = FindTask(nullptr);
    if ((r = check_fresh()))
        return r;
    if (QueryArea(&counter, &where[0]) != 0 || QueryArea((const void *)&local, &stack[0]) != 0)
        return 6;
    set(1000);
    if ((h = CreateTask(second, 2000)) <= 0)
        return 3;
    Wait(SET);
    if (!still(1000))
        return 4;
    set(1001);
    Signal((Handle)h, CHECKED);
    Wait(GONE);
    if (failed)
        return failed;
    if (!still(1001))
        return 5;
    /* Each copy is in the Area its task's locals are in, and they differ. */
    for (unsigned t = 0; t < 2; t++)
        if (where[t].ai_Address != stack[t].ai_Address)
            return 7;
    if (where[0].ai_Address == where[1].ai_Address)
        return 8;
    KPrintF("tls: 2 tasks, each with its own thread_local copy in its own stack, %lu bytes of it",
            (uint64_t)(sizeof zeroed + sizeof counter + sizeof line));
    return 0;
}
