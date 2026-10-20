/*
 * kernel/selftest.h -- boot-time self-tests
 *
 * Each test prints "V1.n <name> ok ..." or "V1.n <name> FAIL ...", or
 * "skipped" where the platform lacks what it tests. A failure stops the
 * boot before the phase's "done" line, which is what tools/boot-check.py
 * waits for. Every check should be mutation-tested: break the code it
 * checks and see the test fail.
 */
#ifndef KERNEL_SELFTEST_H
#define KERNEL_SELFTEST_H

/* V1 step 1: kernel memory, traps, interrupts. */
void selftest_pmm(void);
void selftest_fault(void);
void selftest_timer(void);
void selftest_fdt(void);

/* V1 step 2: SMP. */
void selftest_smp(void);
void selftest_ipi(void);
void selftest_cpu_timers(void);
void selftest_lock(void);

/* V1 step 3: tasks and the scheduler. Runs as a task, all of step 3's
 * tests in turn, then prints "V1.3 done". */
void selftest_tasks(void *);

/* V1 step 4: processes in user mode. Called from selftest_tasks. */
void selftest_user(void);

/* V1 step 5: Areas, in the boot's layout, and the VQ3 soak of both
 * layouts. Called from selftest_tasks. */
void selftest_areas(void);
void selftest_vq3(void);

/* V2 step 1: ports and connections. Called from selftest_tasks. */
void selftest_ipc(void);

/* V2 step 2: rings, with the echo server and its client. */
void selftest_rings(void);

/* V2 step 3: DoMsg's handoff and priority loan, and its cost. */
void selftest_domsg(void);

/* V2 step 4: death notices from both ends, rogue clients, and load. */
void selftest_death(void);

#endif /* KERNEL_SELFTEST_H */
