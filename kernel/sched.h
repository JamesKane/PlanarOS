/*
 * kernel/sched.h -- tasks and the SMP scheduler (docs/v1-plan.md, step 3)
 *
 * Exec's model on every CPU: strict priority from -128 to 127, preemptive,
 * round robin within a priority. A task runs until it blocks, a
 * higher-priority task becomes ready on its CPU, or its 4 ms quantum
 * ends while another task of the same priority waits there. Each CPU has
 * its own run queue; a task belongs to one CPU at a time. New tasks go to
 * the least-loaded CPU their affinity allows, and an idle CPU steals
 * ready tasks that may run on it.
 *
 * Signals are the only way to block (docs §4.1): a task waits for any of
 * a set of signal bits, and anyone may signal it. The low 16 bits are the
 * system's.
 *
 * Locking: a task's state changes only with its CPU's run-queue lock
 * held, and a task's signals with its own lock; the order is task lock,
 * then run-queue lock. A task's context is live while on_cpu is set,
 * from being picked until the switch away from it completes
 * (finish_switch on the next task); no other CPU runs it until then.
 */
#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <stdatomic.h>
#include <stdint.h>

#include "arch.h"
#include "ktime.h"
#include "spinlock.h"

constexpr int TASK_PRI_MIN = -128;
constexpr int TASK_PRI_MAX = 127;
constexpr unsigned TASK_STACK_PAGES = 4;        /* 16 KiB, struct task at its base */
constexpr uint64_t TASK_QUANTUM_US = 4000;
constexpr uint64_t TASK_ANY_CPU = ~(uint64_t)0;

/* Signals the kernel uses for itself. */
constexpr unsigned SIGB_SLEEP = 2;
constexpr uint64_t SIGF_SLEEP = 1ULL << SIGB_SLEEP;
constexpr unsigned SIGB_KILL = 3;           /* wakes a killed task from Wait */
constexpr uint64_t SIGF_KILL = 1ULL << SIGB_KILL;

enum task_state : uint8_t {
    TASK_READY,
    TASK_RUNNING,
    TASK_WAITING,
    TASK_DEAD,
};

struct area;
struct cpu;
struct process;

struct task {
    struct arch_context ctx;
    const char *name;
    int8_t pri;
    int8_t base_pri;                /* pri, less any loan (task_post) */
    enum task_state state;          /* under cpu->rq_lock */
    bool on_rq;                     /* under cpu->rq_lock */
    bool migrating;                 /* leaving its CPU at the next switch */
    atomic_bool on_cpu;             /* context live on some CPU */
    struct cpu *cpu;                /* whose run queue it belongs to */
    uint64_t affinity;              /* CPUs it may run on, by index */
    struct task *rq_next;

    spinlock_t lock;                /* the signals */
    uint64_t sig_received;
    uint64_t sig_waiting;           /* the mask a waiting task wants */

    void (*entry)(void *data);
    void *data;
    uint64_t stack;                 /* physical base of stack + struct; 0 for idle */
    struct ktimer sleep;
    uint64_t runs;                  /* times switched to */

    /* User tasks (kernel/process.c); all zero for a kernel task. */
    struct process *process;
    struct arch_domain *domain;     /* nullptr: the kernel's alone */
    struct task *next_in_proc;
    struct area *user_stack;
    uint32_t handle;
    atomic_bool killed;             /* stop at the next return to user mode */
    uint64_t user_pc, user_sp, user_arg[2];
    uint64_t sig_alloc;             /* AllocSignal's bits */
    _Atomic uint64_t calls;         /* kernel calls it made (cpu_count) */
};

/* Make the code running on this CPU its idle task. Each CPU, once,
 * before it takes interrupts that may schedule. */
void sched_init_cpu(void);

/* This CPU's idle loop: run what's ready, steal what's ready elsewhere,
 * sleep otherwise. Never returns. */
[[noreturn]] void sched_idle(void);

/* A new kernel task, ready to run on a CPU in 'affinity' (and running at
 * once if it outranks what runs there). nullptr if there's no memory or
 * no online CPU in 'affinity'. */
struct task *task_create(const char *name, int pri, uint64_t affinity,
                         void (*entry)(void *), void *data);

/* The same in two steps, so the caller can finish setting the task up
 * before anything runs it; task_discard() frees one never launched. */
struct task *task_new(const char *name, int pri, uint64_t affinity,
                      void (*entry)(void *), void *data);
bool task_launch(struct task *t);
void task_discard(struct task *t);
[[noreturn]] void task_exit(void);
struct task *task_self(void);

/* Change where the calling task may run; it moves at once if it must. */
void task_set_affinity(uint64_t affinity);
void task_set_pri(struct task *t, int pri);

/* The calling task gives back priority lent to it by task_post(). */
void task_end_loan(void);

/* Block until any signal in 'mask' arrives; returns and clears those.
 * A task waiting gives back any priority lent to it. */
uint64_t task_wait(uint64_t mask);
void task_signal(struct task *t, uint64_t mask);

/* task_wait(), then sched_kick('c') if it didn't have to wait (if it
 * did, the switch has done what the kick was for). */
uint64_t task_wait_kick(uint64_t mask, struct cpu *c);

/*
 * task_signal() for a caller that holds a lock: signals 't' and returns
 * the CPU to pass to sched_kick() once the lock is let go, or nullptr.
 * (task_signal() inside a lock works too, but a switch it calls for on
 * this CPU waits for an IPI to itself.) With POST_HANDOFF, a task this
 * wakes that may run on this CPU goes to the front of this CPU's queue,
 * so the caller's next wait switches straight to it; with POST_LEND as
 * well, it runs at the caller's priority if that's higher, until it next
 * signals or waits (docs/v2-plan.md).
 */
constexpr unsigned POST_HANDOFF = 1u << 0;
constexpr unsigned POST_LEND = 1u << 1;

struct cpu *task_post(struct task *t, uint64_t mask, unsigned how);
void sched_kick(struct cpu *c);

/* Exec's SetSignal: set the bits of 'mask' to those of 'bits'; returns
 * the old signals. */
uint64_t task_set_signal(uint64_t bits, uint64_t mask);

/* Let another ready task of the same priority on this CPU run. */
void task_yield(void);

/* Block until time_now() reaches 'deadline'. */
void task_sleep_until(uint64_t deadline);

/* Pick and switch to the best ready task on this CPU. */
void schedule(void);

/* Called by the architecture's trap code around every interrupt; on the
 * way out, switch if the interrupt made a better task ready here or
 * ended the quantum. */
void irq_enter(void);
void irq_exit(void);

/* Counters for the self-tests. */
uint64_t sched_switches(void);
uint64_t sched_steals(void);
uint64_t sched_handoffs(void);

#endif /* KERNEL_SCHED_H */
