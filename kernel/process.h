/*
 * kernel/process.h -- processes: protection domains that own resources
 *
 * A process is a domain, the tasks running in it, and the memory it
 * holds (docs §2.3). It ends by ExitProcess, by a fault in any task, by
 * the kernel killing it, or by its last task ending. Its tasks then stop:
 * one running user code is interrupted and stops on its way back to user
 * mode; one blocked in Wait is woken and stops; one part way through a
 * kernel call finishes it first and stops at the same point (deferred
 * kill). The process is reclaimed once every task has switched away for
 * the last time, so no CPU still has its domain loaded: memory, page
 * tables, ASID and handles go back, and whoever watches it is signalled.
 *
 * Built-in programs (conformance/) are flat images; until V1 step 5 adds
 * Areas and the two layouts, a process's memory is identity-mapped
 * (VA = PA), which also leaves every stack an unmapped guard below it.
 */
#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdatomic.h>
#include <stdint.h>

#include "arch.h"
#include "spinlock.h"

enum fault : uint8_t {
    FAULT_NONE,
    FAULT_READ,
    FAULT_WRITE,
    FAULT_EXEC,
    FAULT_ILLEGAL,
    FAULT_ALIGN,
    FAULT_BREAK,
    FAULT_OTHER,
};

struct task;

struct region {
    uint64_t va, pa;
    uint32_t pages;
};

constexpr unsigned PROCESS_REGIONS = 64;

struct process {
    struct arch_domain domain;
    const char *name;
    int pri;
    spinlock_t lock;                /* tasks, regions */
    struct task *tasks;             /* by next_in_proc */
    atomic_uint unfinished;         /* tasks not yet switched away for good */
    struct region regions[PROCESS_REGIONS];
    unsigned nregions;
    atomic_bool dying;
    atomic_bool dead;               /* reclaimed: only the record is left */
    int64_t exit_code;
    enum fault fault;
    uint64_t fault_pc, fault_addr;
    struct task *watcher;           /* signalled with watch_sig when dead */
    uint64_t watch_sig;
};

/* Load built-in program 'name' into a new process and start it at 'pri'
 * with 'arg' as its first argument. When it's dead, 'watcher' gets
 * 'sig'. nullptr if there's no such program or no memory. */
struct process *process_create(const char *name, int pri, uint64_t arg,
                               struct task *watcher, uint64_t sig);

/* End a process from outside it. Returns at once; the watcher hears
 * when it's dead. */
void process_kill(struct process *p);

/* Free the record of a dead process. */
void process_free(struct process *p);

/* Processes not yet freed, for leak checks. */
unsigned process_count(void);

/* From a task of the process: end it normally, or by a fault. */
[[noreturn]] void process_exit(int64_t code);
[[noreturn]] void process_fault(enum fault kind, uint64_t pc, uint64_t addr);

/* The kernel calls behind CreateTask and ExitTask. */
int64_t process_create_task(uint64_t entry, uint64_t arg);
[[noreturn]] void process_exit_task(void);

/* Last thing on every return to user mode: a killed task stops here. */
void user_return(void);

/* The scheduler's word that a dead task of 'p' has switched away for
 * the last time (finish_switch); the last one reclaims the process. */
void process_task_finished(struct process *p);

#endif /* KERNEL_PROCESS_H */
