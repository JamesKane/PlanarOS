/*
 * lib/exec/task.c -- CreateTask, through crt0's trampoline
 *
 * The kernel starts a task at the address it's given with two argument
 * registers. CreateTask gives it _task_start (crt0.S), the task's
 * argument and its function: the trampoline makes the task's
 * thread_local block (lib/rt/tls.c), calls the function, and ends the
 * task with ExitTask(0) if it returns. The trampoline is the program's,
 * found in its process data (procdata.h), so exec.library's CreateTask,
 * shared, starts the task in the program's own.
 */
#include <planar/exec.h>

#include "procdata.h"

int64_t _CreateTask(uint64_t pc, uint64_t arg, uint64_t entry);   /* the kernel call */

int64_t CreateTask(void (*entry)(uint64_t arg), uint64_t arg)
{
    return _CreateTask((uint64_t)(uintptr_t)process_data()->pd_TaskStart, arg,
                       (uint64_t)(uintptr_t)entry);
}
