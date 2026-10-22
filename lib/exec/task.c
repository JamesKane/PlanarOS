/*
 * lib/exec/task.c -- CreateTask, through crt0's trampoline
 *
 * The kernel starts a task at the address it's given with two argument
 * registers. CreateTask gives it _task_start (crt0.S), the task's
 * argument and its function: the trampoline makes the task's
 * thread_local block (lib/rt/tls.c), calls the function, and ends the
 * task with ExitTask(0) if it returns.
 */
#include <planar/exec.h>

int64_t _CreateTask(uint64_t pc, uint64_t arg, uint64_t entry);   /* the kernel call */
void _task_start(void);

int64_t CreateTask(void (*entry)(uint64_t arg), uint64_t arg)
{
    return _CreateTask((uint64_t)(uintptr_t)_task_start, arg, (uint64_t)(uintptr_t)entry);
}
