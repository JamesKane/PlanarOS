# V1 plan: the SAS core on every CPU

**Goal (§8):** PMM zones, both domain layouts behind `arch_domain_*`, tasks, an SMP scheduler with per-CPU run queues, signals and handles.

**Exit:**
- omega's A1 acceptance tests pass on every CPU, in both layouts.
- Context-switch and `Signal` costs are measured.
- The VQ3 soak and cost data has been collected.

Each step ends with `make test-all` green and a `V1.n done` line in the transcript. `make test-all` also boots AArch64 entered at EL2. The RV2 run comes at the end.

`make PLATFORM=... soak` boots a platform repeatedly and counts failures. Step 3 found a race that hung about one boot in 75: a self-test checked a flag and then executed `wfi` with interrupts on, so an interrupt landing in between left the CPU asleep. Waits now check with interrupts masked. After that, 300 RV64 boots and 400 AArch64 boots (EL1 and EL2) all passed.

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
| **4. Domains and user mode** | `arch_domain_*`, user entry, system calls, the handle table, processes, lazy FP and vector state, reclamation on exit with deferred kill. | yes |
| **5. Areas and the two layouts** | Areas over zoned physical memory. User VA allocators: `identity` (VA = PA) and `global` (a VA window, as in omega), chosen at boot. Port the A1 acceptance tests. Measure a soak test for VQ3. | yes |

## Decision taken in step 1: the kernel stays in the upper half in both layouts

The kernel uses its own direct map in the upper half (Sv39 `0xffff_ffc0_…`, AArch64 TTBR1 `0xffff_0000_…`), mapped global. The two VQ3 layouts differ only in where **user** Areas live: at VA = PA, or in a global VA window.

- This keeps the kernel identical in both layouts, so the measurements compare only the user layout.
- If the kernel's direct map shared the lower half with identity-mapped user pages, both would claim the same addresses. The kernel couldn't use global entries, and every domain would need its own copy of the kernel tables.
- User pointers stay universal: equal to the physical address in the identity layout, and to the global VA in the other. Devices still see the identity value.
- This settles **VQ4**: option A, with the kernel in the upper half.

The cost is that RAM must lie below the top of the user half: 256 GiB for Sv39 and 256 TiB for 48-bit AArch64. All four boards fit.
