/*
 * planar/exec.h -- the kernel calls, as user code sees them
 *
 * Exec's names. These are the raw kernel calls, one stub each
 * (arch/<isa>/user/syscalls.S); exec.library will wrap them. Signals
 * 0-15 are the system's, 16-63 the task's. Handles name kernel objects
 * system-wide.
 */
#ifndef PLANAR_EXEC_H
#define PLANAR_EXEC_H

#include <stdint.h>

typedef uint32_t Handle;

int64_t KLog(const char *text);
[[noreturn]] void ExitProcess(int64_t code);
uint64_t SetSignal(uint64_t bits, uint64_t mask);
uint64_t Wait(uint64_t mask);
int64_t Signal(Handle task, uint64_t mask);
Handle FindTask(const char *name);          /* nullptr: the calling task */
Handle CreateTask(void (*entry)(uint64_t arg), uint64_t arg);
[[noreturn]] void ExitTask(int64_t code);
int64_t AllocSignal(int64_t bit);           /* -1: any free one */
void FreeSignal(int64_t bit);

#endif /* PLANAR_EXEC_H */
