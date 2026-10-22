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
 * A process created by another (CreateProcess) is watched through a
 * handle its creator's process owns, and never outlives that handle:
 * closing it, or the owner dying, kills the process. The record lives
 * while the process does and while its watcher holds it.
 *
 * Built-in programs (conformance/) are flat images. A process's memory
 * is Areas (kernel/area.c): its image (code read and execute, the rest
 * read and write), a stack per task with a guard below, and what it
 * allocates.
 */
#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdatomic.h>
#include <stdint.h>

#include <planar/exec.h>

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

struct mapping;

/* What one process may hold at once (review M11), so none can use up
 * what every process shares. */
constexpr unsigned PROCESS_HANDLES = 256;
constexpr unsigned PROCESS_TASKS = 64;
constexpr unsigned PROCESS_CONNS = 32;  /* connection ends, open or waiting */

struct process {
    struct arch_domain domain;
    const char *name;               /* a built-in's, or name_buf */
    char name_buf[16];              /* a program its parent loaded: CreateProcessImage */
    int pri;
    uint64_t affinity;              /* where its tasks may run, by CPU index */
    spinlock_t lock;                /* tasks */
    struct task *tasks;             /* by next_in_proc */
    unsigned ntasks;                /* on 'tasks' (lock) */
    unsigned handles;               /* held (kernel/handle.c's lock) */
    unsigned conns;                 /* connection ends (kernel/port.c's lock) */
    unsigned klog_tokens;           /* KLog lines it may print now (lock) */
    uint64_t klog_at, klog_dropped;
    atomic_uint unfinished;         /* tasks not yet switched away for good */
    struct mapping *maps;           /* the Areas it maps (kernel/area.c's lock) */
    atomic_bool dying;
    bool maps_closed;               /* being reclaimed: nothing maps in (area.c's lock) */
    bool leases_closed;             /* being reclaimed: nothing is granted (lease.c's lock) */
    atomic_uint refs;               /* the watcher's, and calls using the record */
    atomic_bool dead;               /* reclaimed: only the record is left */
    int64_t exit_code;
    enum fault fault;
    bool fault_mapped;              /* the faulting page was mapped (a rights fault) */
    uint64_t fault_pc, fault_addr;
    struct task *watcher;           /* signalled with watch_sig when dead */
    Handle watch_task;              /* or this task, by handle (CreateProcess) */
    uint64_t watch_sig;
    struct task *held;              /* its first task, until StartProcess */
    atomic_bool killed;             /* ended by process_kill */
    uint64_t stack_base;            /* the first task's stack, for the overrun test */
    atomic_uint_fast64_t calls_done;    /* kernel calls made by tasks that have left */
};

/* Load built-in program 'name' into a new process and start it at 'pri'
 * with 'arg' as its first argument. When it's dead, 'watcher' gets
 * 'sig'. nullptr if there's no such program or no memory. */
struct process *process_create(const char *name, int pri, uint64_t arg,
                               struct task *watcher, uint64_t sig);

/* The same, with every task it ever has confined to 'affinity' (for
 * measuring one core class). */
struct process *process_create_on(const char *name, int pri, uint64_t arg,
                                  struct task *watcher, uint64_t sig, uint64_t affinity);

/* The same, with a device root handle (kernel/lease.h) as its second
 * argument: the first server, or a test acting as one. */
struct process *process_create_root(const char *name, int pri, uint64_t arg,
                                    struct task *watcher, uint64_t sig);

/* End a process from outside it. Returns at once; the watcher hears
 * when it's dead. */
void process_kill(struct process *p);

/* Its handle is gone (CloseHandle, or the owner died): kill it, and let
 * the record go once it's dead. */
void process_release(struct process *p);

/* The process 'h' names, with a reference (process_put), if the calling
 * task's process owns the handle; nullptr otherwise. */
struct process *process_child(Handle h);

/* The watcher's last word on a dead process: its record goes once no
 * kernel call is using it either. process_get/process_put are for those
 * calls (a process found through a task handle). */
void process_free(struct process *p);
void process_get(struct process *p);
void process_put(struct process *p);

/* Kernel calls the process's tasks have made, for the self-tests. */
uint64_t process_calls(struct process *p);

/* Processes not yet freed, for leak checks. */
unsigned process_count(void);

/* From a task of the process: end it normally, or by a fault. */
[[noreturn]] void process_exit(int64_t code);
[[noreturn]] void process_fault(enum fault kind, uint64_t pc, uint64_t addr);

/* The kernel calls behind CreateTask and ExitTask. */
int64_t process_create_task(uint64_t entry, uint64_t arg0, uint64_t arg1);
[[noreturn]] void process_exit_task(void);

/* The kernel calls behind CreateProcess, StartProcess, KillProcess and
 * QueryProcess. */
int64_t process_create_call(uint64_t name, uint64_t arg, int64_t sigbit, uint32_t flags);
int64_t process_create_image_call(uint64_t image, uint64_t arg, int64_t sigbit, uint32_t flags);
int64_t process_start_call(uint32_t process);
int64_t process_kill_call(uint32_t process);
int64_t process_query_call(uint32_t process, uint64_t info_out);

/* Last thing on every return to user mode: a killed task stops here. */
void user_return(void);

/* The scheduler's word that a dead task of 'p' has switched away for
 * the last time (finish_switch); the last one reclaims the process. */
void process_task_finished(struct process *p);

#endif /* KERNEL_PROCESS_H */
