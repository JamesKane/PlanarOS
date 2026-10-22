# 10. Adversarial review of the code through V2

**Status (2026-10-22, V6 step 8):** every critical, high and medium finding has been fixed or, where noted, changed in the docs. Each fix came with a test that fails without it, except M1 (no SVE in QEMU) and M9 (a one-instruction window). The "Now" column gives the commit. Every low finding (§10.8) is now fixed or closed with its reason; "V6.8" marks the ones fixed in V6 step 8, each with a test that fails without it unless the row says why not. M12's `KLog` is a ring now (below).

This document reviews the tree as of `f03eea2` plus the uncommitted changes on 2026-10-20. That covers V0, V1 and V2 steps 1–4 plus the round-trip work. The review assumed a hostile user process, a hostile peer on a connection, and a skeptical reading of the boot inputs. Six passes covered IPC, scheduling/SMP, memory and processes, the rv64 arch layer, the aarch64 and hosted arch layers, and boot plus the syscall boundary. Every critical and high finding was then checked against the code a second time.

Each finding gets a severity and a status:

- **Critical**: a user process can take over the kernel or another process.
- **High**: isolation broken, a protection rule bypassed, or a working program killed or starved.
- **Medium**: a resource attack, a bug that depends on firmware, or a clear contradiction of the docs.
- **Low**: latent, narrow, or hardening only.

The status says how the finding was established:

- **Reproduced**: run in QEMU or a host harness on a scratch copy.
- **Confirmed**: traced in the code; the path is unambiguous.
- **Plausible**: the race or condition is real, but it was not run and it depends on timing or firmware.

Nothing in the repo was changed during the review. `kernel/sched.c` and `kernel/sched.h` did change while the review was running: a DEBUG `magic` check in `finish_switch` was there at the start and is gone now. Findings that touched only that check have been dropped.

## 10.1 Summary

| # | Sev. | Area | Finding | Status | Now |
|---|---|---|---|---|---|
| C1 | Critical | Areas | ShareArea racing FreeArea maps freed physical pages into the caller | Plausible | Fixed `bf60913` |
| H1 | High | aarch64 | EL0 can execute any RAM through the kernel direct map (no UXN/PXN) | Reproduced | Fixed `37d9307` |
| H2 | High | rv64 | `arch_map` changes a live PTE with only a local `sfence.vma` | Confirmed | Fixed `37d9307` |
| H3 | High | rv64 | Vector save and load ignore `vstart`, so another task's v-registers get corrupted | Confirmed | Fixed `8d45cec` |
| H4 | High | IPC lib | `in_heap` checks only the header, so a rogue client kills echo | Reproduced | Fixed `f733561` |
| H5 | High | IPC lib | DoMsg's stash is linked through memory the server can write | Confirmed | Fixed `f733561` |
| H6 | High | rv64 | Vector FP instructions kill the task when FP isn't loaded | Confirmed | Fixed `8d45cec` |
| H7 | High | Sched | An idle CPU is never told about ready work it could steal | Confirmed | Fixed `8c33239` |
| M1 | Medium | aarch64 | SVE traps to EL2 (CPTR_EL2.TZ in RES1); CPACR_EL1 only partly set | Reproduced* | Fixed `8d45cec` (not exercised: no SVE in QEMU) |
| M2 | Medium | aarch64 | EL0 can write the PMU (`PMUSERENR_EL0.EN`) | Reproduced | Fixed `8d45cec` |
| M3 | Medium | aarch64 | `TPIDR_EL0` is never switched and leaks between processes | Reproduced | Fixed `8d45cec` |
| M4 | Medium | Areas | W^X is enforced per mapping, not per Area | Confirmed | Fixed `37d9307` |
| M5 | Medium | Areas | `back_global` leaks the page list on partial failure | Confirmed | Fixed `20ed799` |
| M6 | Medium | aarch64 | ProtectArea's break-before-make window kills sibling tasks | Confirmed | Fixed `b78f6ff` |
| M7 | Medium | Sched/IPC | Priority inheritance only on the sleep→wake edge; no chains | Confirmed | Docs say "handoff boosting" `5233209` |
| M8 | Medium | Sched | Lowering a running task's priority never starts the quantum | Confirmed | Fixed `8c33239` |
| M9 | Medium | Sched | `task_self()` can be read on one CPU and used on another | Plausible | Fixed `8c33239`, `66ac684` (untested: one-instruction window) |
| M10 | Medium | Syscalls | Signal and ShareArea reach any task in any process | Confirmed | Fixed `bf60913` |
| M11 | Medium | Resources | No per-process limits on handles, tasks, connections or arena bytes | Confirmed | Fixed `5233209` (no memory quota yet) |
| M12 | Medium | Syscalls | `KLog` keeps interrupts off for ~19 ms per call | Confirmed | Fixed V6.8: a ring a kernel task drains (`V2.4 klog`); filtered and rate-limited since `13e9e1b` |
| M13 | Medium | Boot | RAM above the direct map is handed to the allocator | Confirmed | Fixed `6326b81` (clip untested by a boot) |
| M14 | Medium | FDT | Length wrap hangs boot; negative depth writes below `stack[]`; header offsets unchecked | Reproduced | Fixed `0f9e38b` |
| M15 | Medium | Boot | The pmm bitmap is written after the kernel before any reservation check | Plausible | Fixed `218f209` |
| M16 | Medium | Build | User runtime objects don't depend on the headers they include | Confirmed | Fixed `b9a17f3` |
| M17 | Medium | Tests | `make test` passes when boot skipped every test | Confirmed | Fixed `b9a17f3` |

\* Reproduced in QEMU with ZEN forced on to stand in for firmware; whether it happens on the CIX board depends on its firmware.

There are 26 low findings in §10.8.

C1 was found independently by two passes, H2 by two and M7 by two.

## 10.2 Critical

### C1. ShareArea or ProtectArea racing FreeArea maps freed pages

`kernel/area.c:368-387` (`area_of`, `process_of`), `:432-447` (`area_free_call`), `:451-463` (`area_share_call`), `:487-505` (`area_protect_call`).

`area_of` and `process_of` return raw pointers from `handle_with` and take no reference. The comment at the top of `area.c` names this as deferred to V2, but V2 now shares Areas across processes without it.

- **Scenario:**
  1. Task 1 of a process calls `ShareArea(h, FindTask(0), R|W)`.
  2. After the lookup and before `map_into`, task 2 calls `FreeArea(h)`. That runs `area_release`, then `area_destroy`, which hands the pages back to the pmm and `kfree`s the record.
  3. `kfree` leaves `pa`/`pages`/`size` intact. `find()` returns NULL because the owner's mapping is gone, so `map_into` links a new mapping. `map_pages` maps the freed run read-write into the caller.
  4. When the pmm reuses those pages for page tables, task structs or another process, the attacker controls them.
- **Retries:** a failed attempt returns BADHANDLE or EXISTS and costs nothing, so it can be retried at will. Handles are predictable (next-fit index, small generation).
- **Same window elsewhere:**
  - `area_alloc_call:416-417`, between `a->owner = p` and `map_into`.
  - Two concurrent `FreeArea(h)` calls: the second walks stale `maps` into a NULL `next_in_proc` in `unlink_mapping:244` and the kernel crashes.
  - `process_of(task)` can return a process that is reclaimed before `map_into`. `walk` then writes PTEs through `table(0)`, i.e. `phys_to_virt(0)`, and the mapping is linked onto a dead process's list.

**Fix:** handles hand out references. Take one on the Area (and on the target process) inside the `handle_with` callback, drop it when the call returns, and free only on the last reference. Inside `map_into`, under the area lock, re-check `a->handle == h` and that the target isn't dying. `reclaim` must take the area lock before it marks a process dead.

## 10.3 High

### H1. aarch64: EL0 can execute any RAM through the kernel's direct map

`arch/aarch64/entry.S:43`, `arch/aarch64/arch.c:25`: `BLOCK_NORMAL = 0x70d` sets neither UXN (bit 54) nor PXN (bit 53).

An EL1-only page (AP[1]=0) is still executable at EL0 when UXN=0. Every TTBR1 RAM block is global and UXN=0, so a user program can execute its own data pages, other processes' pages and the kernel image at `0xffff000000000000 + PA`.

- **Reproduced:** in the identity layout, a user wrote `mov x0,#42; ret` into a static RW buffer and called its direct-map alias. The call returned 42.
- **EL1 as well:** with PXN=0, all of RAM is executable at EL1 too. That turns any kernel control-flow bug into a ret2dir.

**Fix:** set UXN on every TTBR1 descriptor (`entry.S`, `arch.c`, and the attributes `l2_for` copies). Set PXN everywhere except kernel text, and make kernel text read-only.

### H2. rv64: changing a valid PTE is never shot down on other harts

`arch/rv64/mmu.c:196-211`, called from `kernel/area.c:499`.

`arch_map` overwrites the PTE and runs a local `sfence.vma` only. The comment at `area.c:499` ("arch_map breaks before making") is true only on aarch64, and `kernel/arch.h:117-119` promises that mappings take effect on every CPU.

- **Scenario:** in an `AREAF_EXEC` Area, task A on hart 0 caches an RW TLB entry. Task B on hart 1 calls `ProtectArea(h, R|X)`. A keeps writing to a page that is now executable, so W^X is gone.
- **Every downgrade:** RW→R is equally ineffective until hart 0 happens to evict the entry.

**Fix:** if the old PTE was valid, clear it, `fence_all(d->asid, va, PAGE_SIZE)`, then write the new one. That also makes the two ISAs agree.

### H3. rv64: vector save and load ignore `vstart`

`arch/rv64/lazy.S:39-80`.

Whole-register loads and stores honour `vstart`: elements below it are not transferred. Neither routine zeroes it first, the load writes the task's saved `vstart` back last, and user mode can write `vstart` directly.

- **Scenario:**
  1. Task A sets `vstart = k` and is saved. On reload the hart is left with `vstart = k`.
  2. A yields without running any vector instruction, so VS is clean and nothing is saved.
  3. Task B's first vector use on that hart runs `vl8re8.v v0` with `vstart = k`. B's first `k` bytes of v0–v7 keep A's values, which A chose.
- **Other effects:**
  - A's own save with `vstart ≠ 0` loses the low elements.
  - Hardware may raise illegal-instruction for a `vstart` it could never produce. The kernel's own `vl8re8.v` then faults and the kernel halts.

**Fix:** `csrw vstart, zero` after reading it in save, and at the top of load. Restore the saved value only at the very end, as now.

### H4. IPC library: `in_heap` checks only the header

`lib/exec/msg.c:97-100`, `conformance/echo.c:36`.

`in_heap` requires only that the 40-byte `struct Message` fits below `mp_HeapHi`. Echo then writes `em_Value` at offset 40.

- **Reproduced:** a client puts `mp_HeapHi - sizeof(struct Message)` on its ring. On `qemu-virt-a64`, both layouts gave `echo: access fault`, then `V2.4 rogue FAIL`, and every other client lost its server.
- **Worse in the identity layout:** VA = PA there. If the next physical page is another client's arena, the write silently corrupts that client's connection. `docs/v2-plan.md` says a rogue client can scribble only on "its own connection"; this breaks that.

**Fix:** a checked receive, e.g. `GetMsgSized(mp, min_len)`. It reads `mn_Length` once, requires `m + len <= mp_HeapHi`, and echo uses it.

### H5. IPC library: DoMsg's stash is linked through memory the server can write

`lib/exec/msg.c:235-244` (`GetMsg`), `:260-268` (`stash`).

Replies that DoMsg sets aside are chained through `mn_Node.mln_Succ`, which lives in the shared arena. That contradicts the header comment at `msg.c:4-7`.

- **Scenario:** the client does `PutMsg(m1)`, `PutMsg(m2)`, `DoMsg(m3)`. The server replies `m1`, which DoMsg stashes. The server then sets `m1->mln_Succ = X` and replies `m2`. Depending on X:
  - If `*X == 0`, `stash()` writes `m2` to X: a write anywhere in the client.
  - If `X = m1`, `stash()` loops forever.
  - Otherwise, the next `GetMsg` returns X as a `Message *`.

**Fix:** keep the stash in client-private memory, e.g. a small pointer ring in `struct MsgPort` bounded by `mp_Slots`.

### H6. rv64: vector FP instructions kill the task

`arch/rv64/lazy.c:65-66`, `:138-139`.

All of OP-V (0x57) is classed `EXT_VEC`, but OPFVV/OPFVF (funct3 1 and 5, e.g. `vfadd.vv`, `vfmv.f.s`) need FS as well.

- **Scenario:** the task's FP state isn't loaded on this hart.
  1. The first trap loads V.
  2. The retry traps for FS and is classed `EXT_VEC` again.
  3. VS is already on, so `riscv_lazy_first_use` returns false and the process gets `FAULT_ILLEGAL`.
- **Why the tests pass:** `conformance/fpu.c` and `user/fpregs.c` always run `fld` before any vector instruction.

**Fix:** have `first_use` load whichever of FS and VS is still off for OP-V vector-FP encodings, or simply load both.

### H7. An idle CPU is never told about stealable work

`kernel/sched.c:144-157` (`wake_check`), `:616-625` (`task_post`).

- **Scenario:**
  1. Task Y (priority 1) is woken onto CPU A while A runs something at priority 10.
  2. `wake_check` returns false, so no IPI is sent anywhere.
  3. CPU B is idle in WFI. There is no periodic tick (`time_init_cpu` arms `~0`), so B runs `steal()` only if an unrelated interrupt wakes it.
  4. Y waits for as long as A stays busy, possibly for ever, while B is idle.
- **Also:** a task preempted on a busy CPU is stranded in the same way.

**Fix:** when the target CPU won't run the task now, IPI an idle online CPU in its affinity, or enqueue it there directly.

## 10.4 Medium: arch layers

**M1. aarch64 SVE and CPACR setup** (`entry.S:32/81`, `arch.c:72`).
- `CPTR_EL2_RES1 = 0x33ff` includes TZ (bit 8, traps SVE to EL2) and TSM (bit 12). `arch_cpu_init` clears only CPACR_EL1.FPEN, so ZEN, SMEN and TTA keep the firmware's values.
- **Entered at EL2 with ZEN on:** the first SVE instruction traps to EL2, where nothing handles it. QEMU then hung the next boot.
- **Entered at EL1 with ZEN on:** the Z upper halves and P/FFR are never saved, so they leak between tasks.
- **Fix:** write CPACR_EL1 as a complete value, clear TZ/TSM in CPTR_EL2, and handle EC 0x19 (or add SVE state to the lazy area). This matters for CIX, the SVE2 board.

**M2. EL0 can write the PMU** (`arch.c:68`). `PMUSERENR_EL0 = EN|CR`, and EN opens PMCR_EL0, the counter enables, PMCCFILTR and the event types.
- **Reproduced:** a user cleared `PMCR_EL0.E` without trapping. That breaks `arch_read_cycles` for the kernel's benchmarks and every process's `ReadCycles`, and gives processes a covert channel.
- **Fix:** set only CR, and pin PMCCFILTR from EL1.

**M3. `TPIDR_EL0` is never saved or restored** (`vectors.S`, `switch.S`).
- **Reproduced:** one process set the register and the next process read the value back.
- It will clobber thread-local storage as soon as user code uses it.
- **Fix:** switch it per task and zero it in `arch_user_enter`.

**M6. ProtectArea can kill the caller's own process on aarch64** (`arch/aarch64/mmu.c:165-170`, `trap.c:93`).
- Break-before-make leaves the descriptor invalid for a moment. A sibling task that touches the page in that window faults, and aarch64 calls `process_fault` with no spurious-fault check (rv64 has `riscv_fault_spurious`).
- **Fix:** re-walk the domain on a user abort and retry if the access is now permitted.

## 10.5 Medium: memory, scheduling and IPC

**M4. W^X is enforced per mapping, not per Area** (`area.c:391-400`, `:451-463`).
- The owner holds an `AREAF_EXEC` Area RW and shares it R|X. Or it shares RW first and then protects its own mapping to R|X.
- Either way the same pages are writable in one domain and executable in another, without `AREAF_RWX`. `docs/03-memory.md` §3.3 says "never writable at the same time".
- **Fix:** `check_rights` considers every existing mapping of the Area.

**M5. `back_global` leaks the page list** (`area.c:126-137`).
- When `pmm_alloc` fails partway, the pages already taken are freed but `a->pages`/`a->pages_pa` are not.
- A loop of oversized `AllocArea` calls in the global layout drains n/512 pages each time.
- **Fix:** `free_page_list(a)` on that path.

**M7. Priority inheritance is handoff boosting only** (`sched.c:609-626`, `port.c:387, 414-416`, `msg.c:290-298`).
- `POST_LEND` applies only in `task_post`'s TASK_WAITING handoff branch. That branch is also skipped when affinity differs, when the server is `on_cpu`, or when the caller is idle.
- **Busy server:** when a high client sends to a server that is running or ready, nothing is lent. The server serves it at base priority behind any mid-priority hog on its CPU.
- **Chain:** C → S1 → S2. S1's loan ends when S1 waits on S2, and S2's reply lends nothing, so S1 resumes at base while C is still blocked on it.
- `docs/04-ipc.md` §4.4 and `docs/v2-plan.md` promise priority inheritance.
- **Fix:** record the loan on the connection, not on the wake edge, and re-apply it on any post or reply while the lender is still blocked. Otherwise, change the docs to say "handoff boosting".

**M8. Lowering a running task's priority never starts the quantum** (`sched.c:504-508`).
- The RUNNING branch reschedules only when something strictly higher is ready.
- **Scenario:** a server running on a loan calls `SignalConn`, which ends the loan before the handle check, so even a bogus handle works. It drops to the level of a ready peer and then spins. No quantum starts, so the peer never runs on that CPU.
- **Fix:** when `top_level >= level`, set `quantum_check` and kick the CPU.

**M9. `task_self()` can be read on one CPU and used on another** (`sched.c:463-466`, `syscall.c:78-79`).
- Syscalls run with interrupts on. `this_cpu()->current` compiles to two loads; an IRQ between them, preemption and a steal leave `a0` pointing at the old CPU's `struct cpu`.
- **Effect:** the syscall then acts on whatever runs there. A port can be created with the wrong owner process, and `ALLOC_SIGNAL` or `FIND_TASK` can hit the wrong task.
- The window is one instruction, but nothing closes it.
- **Fix:** derive the current task from `sp` (`struct task` sits at the stack base), or read it with interrupts off.

## 10.6 Medium: syscall boundary and resources

**M10. Cross-process Signal and ShareArea** (`syscall.c:53-56`, `handle.c:75-85`).
- `handle_with` doesn't check the owner, and task handles are guessable.
- `Signal(0x10001..0x1ffff, ~0)` raises every non-kernel bit in every task in the system, including connection signals.
- `ShareArea` maps into any process named by a guessed task handle. Combined with C1, that process may already be dead.
- **Fix:** decide which cross-process rights exist, e.g. only through a connection or a handle the caller holds, and check them.

**M11. No quotas** (`handle.c:7`, `port.c:203-333`, `process.c:250-265`).
- The 4096-entry handle table is shared by the whole system. `OpenPort` costs up to 1 MiB of arena plus 64 KiB of rings, and a port's accept queue is unbounded. `CreateTask` has no limit.
- One process can exhaust handles or RAM, after which task, area and connection creation fails everywhere, including in `process_create`.
- **Fix:** per-process quotas and a cap on each port's queue.

**M12. `KLog` keeps interrupts off** (`syscall.c:32-45`, `kprintf.c:19-33`).
- `log_take` disables interrupts and prints through the polled UART: about 19 ms per 215-character line at 115200 baud. Other CPUs spin with interrupts off behind it.
- It also passes raw control and escape bytes to the console.
- **Fix:** a ring the console drains, a rate limit, and a printable-only filter.

## 10.7 Medium: boot, build and tests

**M13. RAM above the direct map** (`memmap.c:37-45`, `pmm.c:68-95`).
- rv64 maps physical 0–16 GiB (`ARCH_DIRECT_MAP_GIB`); aarch64 stops at 512 GiB or when `l2_for` runs out of tables. `pmm` takes all reported RAM anyway.
- `QEMU_MEM=16G` on rv64 places RAM up to 0x4_8000_0000, and the first page above 16 GiB faults the kernel.
- **Fix:** clip RAM ranges to an arch-exported limit.

**M14. FDT parsing trusts the blob** (`kernel/fdt.c`).
- `off = align4(off + 12 + len)` is 32-bit arithmetic. `len = 0xFFFFFFF4` leaves the offset unchanged and boot hangs silently before the console starts (lines 70, 148, 222, 402). *Reproduced* in a host harness.
- Extra `END_NODE` tokens drive `depth` negative, and `parent_of:286` writes below `stack[]`. *Reproduced* with AddressSanitizer.
- `fdt_init` checks only the magic number and version. Block offsets, the reservation list, name and string offsets, and NUL termination of property values are all unchecked.
- The device tree comes from the firmware, which already controls the boot, so these are robustness bugs rather than an attack surface. They still turn one bad blob into a silent hang or stack corruption.
- **Fix:** validate the header against `totalsize`, require every offset to move forward, stop at depth < 0, and bound every string read.

**M15. The pmm bitmap is placed blind** (`pmm.c:89-90`). `memset(bitmap, 0xff, …)` at `round_up(kend)` runs before reservations are applied. A bootloader that puts the DTB or initrd straight after the image has it overwritten. QEMU and the current U-Boot setup put it elsewhere.

**M16. Stale user objects** (`mk/user.mk:15-17, 41`).
- `syscalls.S` and `crt0.S` build without `-MMD`, and the `.d` files for `arch/*/user/*.c` and `lib/rt/string` are generated but never included.
- So renumbering a call in `include/planar/syscalls.h` rebuilds the kernel side but not the user stubs.

**M17. `make test` can pass with nothing tested** (`main.c:88-103`, `selftest.c:44`, `Makefile:53`).
- With no device tree or no managed memory, boot still prints every "Vx done" line. `boot-check.py` checks only for "V2.4 done".
- **Fix:** treat "skipped" as a failure on QEMU platforms.

## 10.8 Low

| Area | Finding | Where |
|---|---|---|
| Processes | **Fixed** `5233209`: `return t->handle` after `launch()`: the new task may already have exited and freed its stack | `process.c:262-264` |
| Areas | **Fixed** V6.8 (share), **closed** (protect): No I-cache sync when sharing EXEC rights, and in protect the sync runs after the RX mapping is live. ShareArea with execute syncs as ProtectArea does; protect's window is seen only by a sibling racing its own process's unpublished code (untested: QEMU's I-cache is coherent) | `area.c:463, 502-504` |
| Areas | **Fixed** V6.8: Oversized AllocArea scans the whole bitmap bit by bit with the pmm lock held and IRQs off. It fails before the scan, and the scan steps a run of the bitmap at a time (`V1.1 pmm`) | `pmm.c:133-164` |
| Areas | **Fixed** `20ed799`: `AREAF_LARGE` + `AREAF_CONTIG` in the global layout: VA 2 MiB-aligned, PA only page-aligned | `area.c:117-118` |
| Areas | **Fixed** V6.8: No ASID rollover (doc §3.6); 8-bit-ASID parts fail process creation after 255 domains. Past the last ASID, domains share one, flushed when a CPU loads a different one with it (`V1.4 asids`, both ISAs) | `aarch64/mmu.c:49-66` |
| rv64 | **Fixed** V6.8: `sstatus.SUM` (and FS/VS) carries over to another task on preemption inside a user copy. Trap entry clears SUM once the frame has it (untested: QEMU can't show a stray kernel access) | `rv64/trap.c:172-187` |
| rv64 | **Fixed** V6.8: `sscratch` never zeroed at boot; firmware garbage makes the first kernel trap look like a user trap. `trap_init` zeroes it on every hart (untested: OpenSBI already does) | `rv64/entry.S:42, 94` |
| rv64 | **Fixed** V6.8: Kernel-mode faults in user copies skip the spurious-fault retry, so they can fail with a bogus copy error. On aarch64 too, which had it: a copy's fault on a page that's there runs again (`conformance/copyrace.c`) | `rv64/trap.c:86-94` |
| rv64 | **Fixed** V6.8: `asid_alloc` loop bound reads `asid_used[1024]` at exhaustion. Rewritten with the shared ASID | `rv64/mmu.c:77` |
| aarch64 | **Fixed** V6.8: CNTKCTL ORed; MDCR_EL2, VMPIDR_EL2, ICC_SRE_EL2, MDSCR_EL1 left at firmware values. CNTKCTL and MDSCR_EL1 written whole, VPIDR/VMPIDR_EL2 the CPU's own; MDCR_EL2 and ICC_SRE_EL2 already were (untested: QEMU's are the reset values) | `arch.c:60`, `entry.S:70-91` |
| aarch64 | **Fixed** V6.8: SCTLR leaves nTWE, UCT, UCI, DZE at 0, so `wfe`, a `CTR_EL0` read or `dc zva` kill the process. UCT and UCI were set in V5, nTWE and DZE now (`conformance/userinsn.c`) | `entry.S:29` |
| aarch64 | **Fixed** V6.8: PSCI asm doesn't declare x1–x17 clobbered (SMCCC v1.0 allows it). Untested: no firmware here clobbers them | `arch.c:88-97` |
| aarch64 | **Fixed** V6.8: No kernel stack guard; overflow silently overwrites `struct task` at the stack base. Both ISAs: a canary, `struct task`'s last word, checked at every switch; the stacks are in the direct map's blocks, so no guard page (`V1.3 stack`) | `vectors.S:18`, `sched.h:34` |
| aarch64 | **Closed**: SGI 0 reads as "nothing pending", so it is never EOI'd. SGI 0 is never enabled or sent: IPIs are SGI 1, and `irq_attach` refuses source 0 | `gicv3.c:170`, `irq.c:31` |
| Host | **Closed**: IRQ ops are no-ops, copies always fail, one CPU: hides contract violations. The hosted build is for the parsers, ACPI and the sanitizers; QEMU's SMP boots cover masking and ordering | `arch/host/arch.c:55-74, 185-203` |
| Sched | **Fixed** `e8055d8`: `set_pri` races the enqueue of a migrating task, so the queue level is wrong (selftest-only today) | `sched.c:481-511, 229-239` |
| Sched | **Fixed** V6.8: Affinity to no online CPU loses a migrating task, and its stack leaks. `task_set_affinity` refuses it (`V1.3 migrate`) | `sched.c:230-239` |
| Sched | **Fixed** `268591d` (after the sleep timer, `e8055d8`): `ktimer_stop` doesn't wait for a handler already running, so it can use a freed task (latent). It waits now for one running on another CPU | `ktime.c:105-141` |
| Sched | **Closed**: A woken task can run once outside its affinity. Only a selftest can change a waiting task's affinity | `sched.c:622-623` |
| Sched | **Fixed** V6.8: `cpu_count` with interrupts on, and the global `taken`, lose updates; this loosens the V2.2/V2.3 call-count bounds. The call count is taken with interrupts off, and `taken` is atomic (untested: a race) | `syscall.c:78`, `irq.c` |
| Sched | **Fixed** V6.8: `time_to_us` overflows (`ticks * 1e6`), and selftest passes it an absolute time. `time_us` too; no caller passes an absolute time now (`V1.1 timer`: a year both ways) | `ktime.c:58-61`, `selftest.c:738` |
| IPC | **Closed**: A lent priority outlives the request when the reply needs no call. The loan ends at the server's next wait, which every server loop reaches | `port.c:387`, `sched.c:561` |
| IPC | **Fixed** V6.8: Echo's accept loop stops at the first error, stranding queued clients; 8 idle connections lock it out. `AcceptPort` closes a broken client and takes the next; echo and input close one past their places (`conformance/crowd.c`) | `echo.c:28-30` |
| IPC | **Fixed** V6.8: `ClosePort` from a sibling task leaves a DoMsg waiter in that process asleep for ever. The kernel wakes the end's waiter, which sees the port broken before touching the arena (`conformance/deadline.c`); a sibling already inside the arena still faults: a MsgPort is one task's, as in the RKM | `port.c:129-146` |
| IPC | **Fixed** V6.8: Port names are first-come and global, so any process can impersonate "echo". Naming a port takes a right, `PROCF_NAMES`: the kernel's processes have it, built-in programs inherit it, a program loaded from a disk has it only if given it (dostest's `C:hello`) | `port.c:203-234` |
| Boot | **Fixed** V6.8: Memory nodes matched by name only; a wrapping reservation is silently dropped. A memory node must say `device_type = "memory"` and not be disabled, and a range past 2^64 is reserved to the top (untested: a dtb of its own costs more than it shows) | `memmap.c:37-52`, `pmm.c:41-61` |

## 10.9 Test gaps

- **The rogue cases** (`selftest.c:1516`, `rogue.c`) cover a bad pointer, a broken ring and slot scribbling. They don't cover:
  - a message crossing the heap end (H4)
  - a poisoned stash (H5)
  - header tampering before accept
- **The DoMsg set-aside path** is never run: every client keeps one message in flight.
- **Priority lending** is checked only for a sleeping kernel-task server. Nothing tests a busy server, a chain, or lowering a priority with an equal-priority peer ready.
- **`test_handoff`** with two CPUs has `other == k`, so "runs on the waker's CPU" proves nothing below three CPUs.
- **`post_and_wait`** still uses the old wait-then-kick sequence. The new `task_wait_kick` path is reached only through user DoMsg.
- **The FP/vector conformance tests** always use scalar FP before vector, which hides H6.
- **`conn.c`'s leaked-connection check** depends on a 5 ms sleep rather than waiting for the death notice.
- **The hosted build** can't fail on interrupt-masking or SMP-ordering bugs (§10.8).

## 10.10 Checked and found sound

- User-pointer bounds in `arch_copy_*_user` on both ISAs: aarch64 uses `ldtrb`/`sttrb` with PAN, and rv64 uses SUM plus the `USER_TOP` check.
- Unmap shootdowns (SBI RFENCE, `tlbi …is`); an ASID is freed only after its flush; no CPU keeps a dying domain loaded.
- pmm double-free detection; `va.c` split and merge.
- `port.c`/`handle.c` lifetimes, the lockless `handle_get` invariant (worth an assert), the `waiting` protocol's fences, and ring index checks.
- The `on_cpu` handoff guard (the deadlock fix), the `finish_switch` read-before-release fix, the lock order (task → rq → timer), and the idle WFI wakeup.
- Lazy FP ownership across CPUs on both ISAs (the `vstart` and vector-FP problems above aside).
- Trap entry and exit on both ISAs, SBI and PSCI encodings, PLIC and GIC claim/EOI ordering, and the K1 watchdog sequence.
- Kernel string routines don't compile into self-calls. The build has `-ffreestanding -fno-builtin -fno-stack-protector`, plus `-mgeneral-regs-only` on aarch64 and no F/D on rv64.

## 10.11 Suggested order

1. **C1 and M10 together:** references in handles, and owner checks on cross-process calls. This is the V2 item the `area.c` header already promises.
2. **H1, H2 and M4:** the W^X and protection holes. Each is small and local.
3. **H4 and H5:** the library is the model every server copies, so fix it before more servers are written.
4. **H3, H6 and M1–M3:** per-task register state on both ISAs, with conformance cases that use vector FP first and set `vstart`/`TPIDR_EL0`.
5. **H7, M7 and M8:** scheduler liveness, and either real inheritance or the docs changed to say "handoff boosting".
6. **M16 and M17 before the next round,** so the tests can be trusted.
