# V1 plan: the SAS core on every CPU

**Goal (§8):** PMM zones, both domain layouts behind `arch_domain_*`, tasks, an SMP scheduler with per-CPU run queues, signals and handles.

**Exit:**
- omega's A1 acceptance tests pass on every CPU, in both layouts.
- Context-switch and `Signal` costs are measured.
- The VQ3 soak and cost data has been collected.

Each step ends with `make test-all` green and a `V1.n done` line in the transcript. `make test-all` also boots AArch64 entered at EL2.

**On the Orange Pi RV2 (2026-10-19): the whole V1 suite passes in both layouts.** That covers 8 harts, PLIC contexts from U-Boot's tree, the 24 MHz timebase, user mode, lazy vector state at VLEN 256, Areas, and VQ3 scaled to 8 GiB.

The board runs unattended: `make PLATFORM=ky-x1 test LAYOUT=… BOOTARGS='reboot [vq3]'`.
- The kernel takes over the SoC watchdog that U-Boot leaves running (`plat/ky-x1/wdt.c`, from the vendor driver's sequences).
- A finished run resets the board through the watchdog. OpenSBI 1.3's SBI SRST hangs on this board.
- A hung kernel is reset within a minute. `BOOTARGS=hang` checks that.

First numbers on the board, X60 at 1.6 GHz:

| Measurement | Result |
|---|---|
| Signal/Wait round trip, same CPU (two switches) | 1.46 µs, about 2,340 cycles (omega's `DoMsg`: 3,361) |
| The same across CPUs | 6.6 µs |
| Bare IPI round trip | 2.0 µs |
| Higher-priority task preempting a busy CPU | 36 µs |
| Killing 3 spinning tasks | gone in 73 µs |

`make PLATFORM=... soak` boots a platform repeatedly and counts failures. Step 3 found a race that hung about one boot in 75: a self-test checked a flag and then executed `wfi` with interrupts on, so an interrupt landing in between left the CPU asleep. Waits now check with interrupts masked. After that, 300 RV64 boots and 400 AArch64 boots (EL1 and EL2) all passed.

Step 4a's design points:
- **Reclaiming on SMP.** A process is reclaimed only once each of its tasks has switched away for the last time. The scheduler's `finish_switch` counts them down. Until then, some CPU might still be walking the process's page tables.
- **Kernel tasks run in the kernel's domain.** So a domain is loaded only while one of its tasks runs.
- **RISC-V `tp`.** A trap from user mode finds its CPU in a slot above the trap frame, because user code owns `tp`. A return to the kernel never restores `tp` from the frame, because a preempted task may resume on another CPU. That was a latent bug in step 3.
- **RISC-V TLBs.** `sfence.vma` is local, so unmaps and domain teardown fence every hart through SBI RFENCE. A page fault on a page another hart just mapped is recognised and retried.
- **AArch64 PAN.** With `SCTLR.SPAN` clear, PAN is set on every exception to EL1. User copies use `ldtrb`/`sttrb`, which are checked with EL0 permissions.
- **Testing.** Mutation tests confirmed the step 4a checks catch W^X turned off, missing kill IPIs, and leaked process memory.

Step 5 delivers:
- **Areas** (omega's calls) over both layouts. The layout is chosen at boot with `layout=identity|global`.
- **The A1 tests that need no IPC:** `areas` (omega's, less the parts that use ports), `overrun`, `rofault`, and cross-process sharing tested from the kernel. The A1 tests built on ports (`echo`, `client`, `pingpong`) move to V2.
- **The VQ3 soak** (`make vq3`). The results are in [vq3-results.md](vq3-results.md).

Step 5 also found and fixed a RISC-V bug, which omega has too. A new task's first user frame carried `SIE=1`, so `trap_return` turned interrupts on just before its `sret`. An interrupt in that window took itself for a trap from user mode and overwrote the frame being restored, sending the task to user mode at a kernel address, about one boot in 300. `trap_return` now always writes `sstatus` with `SIE` clear. After the fix, 950 boots across both ISAs and both layouts all passed.

Step 4b's design: registers are saved eagerly, at the switch away and only if they were written, and loaded lazily, on first use. So no CPU ever holds state a task needs on another CPU. The load is skipped when this CPU still holds the task's registers and the task hasn't run anywhere else since (`lazy_cpu`). AArch64 has no dirty bit, so having access turned on counts as written. The QEMU counters show the mechanism working but don't settle eager vs. lazy (§7.4). That waits for boards running real programs.

What step 3 delivers, and what it doesn't yet:
- **Delivered:** per-CPU run queues with 256 priorities; preemption by IPI; a 4 ms quantum running only while a peer waits; signals; self-migration; idle CPUs stealing ready tasks; placement on the least-loaded CPU in a task's affinity.
- **Not yet:** a placement *policy* by core class. Each CPU's `capacity-dmips-mhz` is read, but nothing uses it. That needs big.LITTLE hardware to test on, so it waits for a board.
- **Not yet:** moving another task between CPUs. Only a task can move itself.

Step 2's first QEMU numbers are under TCG, so they say nothing about the hardware. They only show the paths work: cross-CPU call round trip about 8–14 µs, and every CPU online whichever RISC-V hart OpenSBI booted.

| Step | Content | Both ISAs? |
|---|---|---|
| **1. Kernel memory, traps, interrupts** ✅ | Upper-half kernel with a direct map (Sv39 gigapages; AArch64 TTBR1 1 GiB blocks, with RAM split from devices by the device tree). Trap entry and return with fixups, and Guru Meditation on kernel faults. Physical page allocator (omega's bitmap, made SMP-safe). An interrupt controller picked from the device tree (PLIC, GICv3). Tickless timer (Sstc/SBI, generic virtual timer). | yes |
| **2. SMP** ✅ | Start the other CPUs (SBI HSM, PSCI `CPU_ON`). Per-CPU data, spinlocks, IPIs (SBI IPI, GIC SGIs), per-CPU timers and interrupt routing. | yes |
| **3. Tasks and scheduler** ✅ | Kernel tasks; per-CPU run queues with Exec's priorities (−128..127, round robin within a level); preemption; placement by core class; migration and load balancing; signals (`Wait`, `Signal`, `SetSignal`). Measure context switches and cross-CPU `Signal`. | yes |
| **4a. Domains and user mode** ✅ | `arch_domain_*` (Sv39; VMSAv8-64 TTBR0 with ASIDs), user entry, system calls, the handle table, processes from built-in images, faults, reclamation on exit, deferred kill across CPUs. User memory is identity-mapped until step 5. | yes |
| **4b. Lazy FP and vector state** ✅ | `sstatus.FS`/`VS` on RISC-V; first-use trap through `CPACR_EL1.FPEN` on AArch64, with an eager-vs-lazy measurement (§7.4). User programs get FP and vector. | yes |
| **5. Areas and the two layouts** ✅ | Areas over zoned physical memory. User VA allocators: `identity` (VA = PA) and `global` (a VA window, as in omega), chosen at boot. Port the A1 acceptance tests. Measure a soak test for VQ3. | yes |

## Decision taken in step 1: the kernel stays in the upper half in both layouts

The kernel uses its own direct map in the upper half (Sv39 `0xffff_ffc0_…`, AArch64 TTBR1 `0xffff_0000_…`), mapped global. The two VQ3 layouts differ only in where **user** Areas live: at VA = PA, or in a global VA window.

- This keeps the kernel identical in both layouts, so the measurements compare only the user layout.
- If the kernel's direct map shared the lower half with identity-mapped user pages, both would claim the same addresses. The kernel couldn't use global entries, and every domain would need its own copy of the kernel tables.
- User pointers stay universal: equal to the physical address in the identity layout, and to the global VA in the other. Devices still see the identity value.
- This settles **VQ4**: option A, with the kernel in the upper half.

The cost is that RAM must lie below the top of the user half: 256 GiB for Sv39 and 256 TiB for 48-bit AArch64. All four boards fit.
