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

#endif /* KERNEL_SELFTEST_H */
