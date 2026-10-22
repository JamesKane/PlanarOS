/*
 * lib/exec/procdata.h -- what exec.library keeps for a process, in the
 * process's own memory
 *
 * A shared library is code and constants (V6 step 4), so what exec
 * needs per process lives in the program: AllocVec's pool, and crt0's
 * trampoline for CreateTask, which only the program can know (its
 * thread_local layout is its own). lib/rt/tls.c puts a pointer to the
 * program's block in the first word of every task's thread block,
 * which on Arm is at the thread pointer and on RISC-V 64 bytes below it
 * (arch/<isa>/user/procdata.c reads it).
 * The layout is the ABI between a program and exec.library: grow it
 * only at the end.
 */
#ifndef LIB_EXEC_PROCDATA_H
#define LIB_EXEC_PROCDATA_H

#include <planar/exec.h>

struct ProcessData {
    struct Pool *_Atomic pd_VecPool;    /* AllocVec's, made at its first call */
    void (*pd_TaskStart)(void);         /* crt0's _task_start */
};

#if __STDC_HOSTED__
/* The host's tests: one process, no thread block. */
static struct ProcessData host_process_data;

static inline struct ProcessData *process_data(void)
{
    return &host_process_data;
}
#else
/* The calling task's thread block's first word (arch/<isa>/user/procdata.c). */
struct ProcessData *process_data(void);
#endif

#endif /* LIB_EXEC_PROCDATA_H */
