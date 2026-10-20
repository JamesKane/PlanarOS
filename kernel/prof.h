/*
 * kernel/prof.h -- where the cycles go (make PROFILE=1)
 *
 * prof_mark(point) at fixed places on the kernel-call and switch paths.
 * Each CPU adds the cycles since its previous mark to the pair (previous
 * point, this point), so a round trip reads as the edges it took and
 * what each cost; the time in user mode is the edge from leaving the
 * kernel to entering it again. Built only with PROFILE=1: otherwise
 * every mark is nothing.
 */
#ifndef KERNEL_PROF_H
#define KERNEL_PROF_H

enum prof_point : unsigned {
    P_TRAP_IN,          /* trap_handler, from user mode */
    P_SYSCALL,          /* syscall_dispatch, before the call */
    P_SYSCALL_OUT,      /* after it */
    P_TRAP_OUT,         /* about to return to user mode */
    P_IPC_POSTED,       /* SignalWaitConn: peer posted, lock let go */
    P_WAIT,             /* task_wait: about to block */
    P_SCHED,            /* schedule() */
    P_SWITCH,           /* just before arch_switch */
    P_SWITCHED,         /* just after it, as the next task */
    P_FINISHED,         /* finish_switch done */
    P_SCHED_OUT,        /* schedule() returning */
    P_KICK,             /* sched_kick */
    P_X1, P_X2, P_X3, P_X4,    /* for digging: wherever they're needed now */
    P_POINTS
};

#ifdef PLANAR_PROF
void prof_mark(enum prof_point p);
void prof_reset(void);
void prof_report(const char *what, unsigned long per);
#else
static inline void prof_mark(enum prof_point) {}
static inline void prof_reset(void) {}
static inline void prof_report(const char *, unsigned long) {}
#endif

#endif /* KERNEL_PROF_H */
