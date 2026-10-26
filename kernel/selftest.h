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

/* V1 step 5: Areas, and the VQ3 soak. Called from selftest_tasks. */
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

/* V3 step 1: how the kernel was booted, and the EFI memory map if it
 * came through UEFI. Called from selftest_tasks. */
void selftest_boot(void);

/* V3 step 2: the choice of firmware description, and the ACPI parser
 * against good and broken tables. Called from selftest_tasks. */
void selftest_acpi(void);
void selftest_board(void);
void selftest_classes(void);

/* V3 step 5: what the kernel's paths cost on each core class. Called
 * from selftest_tasks. */
void selftest_costs(void);

/* V3 step 6: user mode's TLB misses (VQ3). Called from selftest_tasks. */
void selftest_tlb(void);

/* V4 step 1: the kernel calls drivers need. Called from selftest_tasks. */
void selftest_processes(void);
void selftest_time(void);
void selftest_leases(void);
void selftest_hwdb(void);

/* V5 step 1: the kernel calls queue leases need. Called from selftest_tasks. */
void selftest_futex(void);
void selftest_uncached(void);
void selftest_shares(void);

/* V6 steps 1 to 3: the runtime floor (compiler-rt, mem*, thread_local),
 * exec.library's memory and formatting, utility.library. */
void selftest_runtime(void);
void selftest_exec_memory(void);
void selftest_utility(void);
void selftest_libraries(void);
void selftest_clib(void);
void selftest_dos_args(void);
void selftest_supervision(void);
void selftest_debts(void);
/* V7 step 2: gfx.library. */
void selftest_gfx(void);
void selftest_font(void);

#endif /* KERNEL_SELFTEST_H */
