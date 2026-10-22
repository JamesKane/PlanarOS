/*
 * kernel/cpu.h -- the CPUs, and what each one keeps for itself
 *
 * CPUs come from the firmware description (/cpus, or the MADT). Index 0
 * is always the CPU that booted, which on RISC-V is whichever hart won
 * OpenSBI's lottery; the rest follow in the description's order. this_cpu() is one
 * register read (tp on RISC-V, TPIDR_EL1 on AArch64).
 */
#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "ktime.h"
#include "spinlock.h"

constexpr unsigned MAX_CPUS = 16;
constexpr uint32_t CPU_NO_CLUSTER = ~0u;

struct cpu_call;
struct task;

/* One more in a statistic only one CPU (or task) writes and others may
 * read: no atomic read-modify-write, which costs on every CPU. */
static inline void cpu_count(_Atomic uint64_t *n)
{
    atomic_store_explicit(n, atomic_load_explicit(n, memory_order_relaxed) + 1,
                          memory_order_relaxed);
}

struct cpu {
    uintptr_t stack_top;        /* first: the secondary entry code loads it */
    struct task *current, *idle;    /* near the start: arch_current() loads it in one go */
    unsigned index;
    uint64_t hwid;              /* hart ID, or MPIDR Aff2.Aff1.Aff0 */
    int32_t node;               /* its /cpus node; -1 from ACPI */
    atomic_bool online;
    atomic_uint boot_step;          /* how far a secondary got (kmain_secondary) */

    spinlock_t timer_lock;      /* kernel timers armed on this CPU */
    struct ktimer *timers;
    struct ktimer *_Atomic timer_running;  /* whose handler runs now */

    spinlock_t call_lock;       /* cross-CPU calls waiting to run here */
    struct cpu_call *calls;
    atomic_uint_fast64_t ipis;  /* IPIs taken */
    _Atomic uint64_t syscalls;  /* kernel calls made here (cpu_count) */
    _Atomic uint64_t switches;  /* task switches here (cpu_count) */
    _Atomic uint64_t crossings; /* of those, into another process's domain */
    _Atomic uint64_t handoffs;  /* tasks handed this CPU (cpu_count) */

    /* The run queue (kernel/sched.c): one FIFO per priority, and a bit
     * per non-empty one. */
    spinlock_t rq_lock;
    uint64_t rq_bits[4];
    struct task *rq_head[256], *rq_tail[256];
    unsigned nr_ready;
    struct task *switched_from; /* for finish_switch, across arch_switch */
    unsigned irq_depth;
    atomic_bool need_resched;   /* set by a wakeup or the quantum */
    atomic_bool quantum_check;  /* a peer of current may be waiting */
    struct ktimer quantum;
    bool quantum_on;
    uint64_t type;              /* core type: arch_cpu_type(), read on the CPU itself */
    uint32_t cluster;           /* the PPTT's cluster ID; CPU_NO_CLUSTER if none says */
    unsigned cls;               /* its core class (kernel/cpuclass.c) */
    uint64_t work_ticks;        /* the class workload's best time here, in timer ticks */
    uint64_t capacity;          /* its class's speed; the fastest class is 1024 */
    const void *loaded_domain;  /* arch: the domain this CPU has loaded */
    const void *shared_last;    /* arch: the last loaded with the shared ASID */
    struct arch_cpu arch;       /* arch: lazily switched register state */
};

static_assert(offsetof(struct cpu, stack_top) == 0, "entry code reads stack_top at 0");
static_assert(offsetof(struct cpu, current) < 2048, "arch_current() needs a short offset");

extern struct cpu cpus[MAX_CPUS];
extern unsigned ncpus;

/* The CPU this runs on. Valid from the first line of kmain(). */
static inline struct cpu *this_cpu(void);

/* Read /cpus, or the MADT's enabled CPUs. The boot CPU, already running
 * as cpus[0], is found by its hardware ID. False if the description
 * doesn't list it. */
bool cpu_enumerate(uint64_t boot_hwid);

/* Start every other CPU and wait for each to come online. Returns how
 * many are online, the boot CPU included. */
unsigned smp_start(void);

/* How many CPUs are online now. */
unsigned cpus_online(void);

/*
 * Run fn(arg) on CPU 'c' in its IPI handler, with interrupts off there.
 * With 'wait', returns once it has run; otherwise 'call' must outlive
 * the call (static or heap). On this CPU it just runs fn.
 */
struct cpu_call {
    void (*fn)(void *arg);
    void *arg;
    struct cpu_call *next;
    atomic_bool done;
};

bool cpu_call(struct cpu *c, struct cpu_call *call, bool wait);

/* The IPI arrived: run the calls queued for this CPU. */
void ipi_handle(void);

/* Take IPIs on this CPU (each CPU, once). */
void ipi_init_cpu(void);

#include <planar/arch_cpu.h>

#endif /* KERNEL_CPU_H */
