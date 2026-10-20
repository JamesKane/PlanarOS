/*
 * conformance/tasks.c -- several tasks in one process: each signals the
 * first and ends; the first waits for all of them
 */
#include <planar/exec.h>

#include "log.h"

constexpr int WORKERS = 3;
static Handle main_task;

static void worker(uint64_t n)
{
    Signal(main_task, 1ULL << (20 + n));
    ExitTask(0);
}

int main(void)
{
    uint64_t want = 0, got = 0;
    struct logbuf b = { 0 };

    main_task = FindTask(nullptr);
    for (int n = 0; n < WORKERS; n++) {
        want |= 1ULL << (20 + n);
        if (CreateTask(worker, (uint64_t)n) <= 0)
            return 1;
    }
    while (got != want)
        got |= Wait(want);
    log_str(&b, "all ");
    log_num(&b, WORKERS);
    log_str(&b, " workers signalled");
    KLog(b.text);
    return 0;
}
