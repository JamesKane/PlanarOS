/*
 * lib/rt/tls.c -- each task's thread_local variables
 *
 * The linker gathers them into .tdata, with their first values, and
 * .tbss, zeroed (user.ld), aligned to 64 bytes. Every task has its own
 * block of them at the top of its stack: crt0 makes the first task's,
 * and _task_start (crt0.S) each CreateTask's, then points the thread
 * register at it, which the kernel switches with the task (tp on RISC-V,
 * TPIDR_EL0 on Arm). The variables start 'tcb' bytes into the block:
 * crt0 passes 64 on both ISAs. On Arm the linker leaves 16 bytes,
 * aligned to the block's 64, and the thread pointer is the block; on
 * RISC-V the variables start at the thread pointer, so crt0 points it
 * 64 bytes in.
 *
 * The block's first word points at the program's process data
 * (lib/exec/procdata.h), which exec.library reads, and this file holds
 * that and IExec, which crt0 sets from the kernel's third argument.
 */
#include <stddef.h>
#include <stdint.h>

#include "../exec/procdata.h"

void _task_start(void);

struct ExecIFace *IExec;
static struct ProcessData process = { .pd_TaskStart = _task_start };

/* From user.ld; hidden, so reached PC-relatively. */
extern const uint8_t __tdata_start[] __attribute__((visibility("hidden")));
extern const uint8_t __tdata_end[] __attribute__((visibility("hidden")));
extern const uint8_t __tbss_end[] __attribute__((visibility("hidden")));

uint64_t _tls_size(uint64_t tcb);
void *_tls_init(uint8_t *block, uint64_t tcb);

/* Bytes a task's block takes: a multiple of 64, so the stack below it
 * stays aligned. */
uint64_t _tls_size(uint64_t tcb)
{
    return (tcb + (uint64_t)(__tbss_end - __tdata_start) + 63) & ~63ULL;
}

/* Fill a block at 'block' (64-byte aligned, _tls_size bytes): the
 * thread register's value. Stacks aren't zeroed, so .tbss is. */
void *_tls_init(uint8_t *block, uint64_t tcb)
{
    const uint8_t *s = __tdata_start;
    uint8_t *d = block + tcb;

    process.pd_IExec = IExec;           /* crt0 sets it first */
    *(struct ProcessData **)block = &process;
    while (s < __tdata_end)
        *d++ = *s++;
    for (uint64_t n = (uint64_t)(__tbss_end - __tdata_end); n; n--)
        *d++ = 0;
    return block;
}
