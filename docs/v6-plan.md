# V6 plan: foundations, libraries and supervision

**Status: in progress (2026-10-22): steps 1 and 2 done.** Scoped from the [roadmap's replan after V5](08-roadmap.md#replanned-after-v5): §17 (the C runtime and libraries), §7.2 and §3.4 (interface tables and the shared library region), §11 and §12 (the driver model's and let-it-crash's unbuilt halves), §10.8 (the review's open low findings), and what V5 carried forward. The decisions at the end are open.

**Goal (§8):** the floor the Workbench stands on, and the supervision the design set promised. Every library V7 and V8 add (`gfx`, `ui`, `font`, `intuition`'s client side) is a shared library behind an interface table, built on pools, `Span` and checked readers. None of that exists. And V5 showed what happens without deadlines and supervision: the RV2's `dos` stall left `gpu-rogue` waiting forever on a call that couldn't time out.

**What's already built.**
- `lib/rt`: relocation and byte-at-a-time `memcpy`, `memmove`, `memset`, `memcmp`. `lib/exec`: the message library (`msg.c`) and the kernel calls' stubs. `lib/dos`: dos.library in snake_case (`dos_open`, `dos_args`, `con_puts`). `lib/blk`, `lib/drv`.
- Every program is a static PIE in the kernel's boot bundle (`user_images[]`), or loaded by `dos_run`. Libraries are `.a` files linked into each (`mk/user.mk`).
- acpid has its own `clib.c` and `heap.c` for ACPICA.
- hwdb is the kernel's root process. It supervises the drivers one for one with backoff, and keeps `dos`, `input`, `display` and `power` going. When hwdb ends, the kernel logs it, and reboots only with the boot argument `reboot`.
- Unbinding a driver is killing it (`drv.h`: "no STOP message yet").
- Clients reconnect to a restarted server by polling `OpenPort` for `ERR_NOTFOUND` (`lib/blk`, `gputest`, `displaytest`).
- `DoMsg` has no deadline. Neither does anything built on it: dos.library, the power votes, the GPU and display classes.
- `TPIDR_EL0` is saved and restored per task (V2.5). Nothing uses it, nor RISC-V's `tp`.

**Not built, and needed here:**
- compiler-rt's 128-bit builtins (`udivti3` and friends). Clang emits calls to them for `__int128` and `_BitInt` division, and §17's overflow-checked `Span` readers need them.
- A loader that binds a program to libraries it didn't link, and a place where a library's code lives once for every process.
- A way to wait for a port's name, a way to give up on a reply, and someone watching hwdb.

**Exit (§8):**
- `conformance/pool.c` and `conformance/utility.c` pass on both ISAs. The 128-bit builtins are linked, and checked against the compiler's own arithmetic. `mem*` move 8 bytes a step or more, and SIMD where the CPU has it.
- `exec.library` and `utility.library` are reached through their interface tables (`IExec->`, `IUtility->`) from two processes. Their code is mapped once from the shared library region: each process's page count says so.
- acpid builds on `lib/clib`, and its `clib.c` and `heap.c` are deleted. Every command in `cmd/` takes its arguments by `ReadArgs`. dos.library has its RKM names.
- A client of a killed `dos`, `power` or class driver reconnects without polling (`OpenPort`'s wait). Every call to a core server has a deadline. A driver asked to `STOP` lets go of its device and exits, and a driver that doesn't is killed when the deadline passes.
- `init` is the root process. It starts hwdb, and hwdb's death is caught: logged as a meditation, then the board is reset by its watchdog.
- The review's open low findings (§10.8) are fixed or closed with a reason. `KLog` no longer spins with interrupts off.
- The kill test passes 10 runs in a row on each board, which takes the RV2's `dos` stall found and fixed.
- The manuals are current (roadmap rule), with *Libraries* chapters on `utility.library` (5), pools (in 3) and interface tables (8), and the dos rename in the autodocs.

Each step ends with `make test-all` green and a `V6.n done` line in the transcript.

| Step | Content |
|---|---|
| **1. The runtime floor** | compiler-rt's builtins vendored; `mem*` by words, then SIMD (RVV, NEON); `CopyMem`, `CopyMemQuick`, `SetMem`; `thread_local` |
| **2. exec.library's memory and formatting** | V39 pools, V36 `AllocVec`/`FreeVec`, `RawDoFmt` |
| **3. `utility.library`** | `TagItem`, `Hook`, string and case helpers, `SNPrintf`, `Span`/`Str` and the checked endian readers |
| **4. Interface tables and the shared library region** | Libraries as images mapped once into every domain; `OpenLibrary` and interface tables; how a process finds `IExec` (VQ6); SIMD chosen through the table (VQ22) |
| **5. `lib/clib`** | An opt-in C library on steps 2 and 3; acpid moved onto it |
| **6. dos.library in RKM names** | The rename, `ReadArgs`, `FilePart`/`PathPart`/`AddPart`, `Printf`/`PutStr`; every command moved |
| **7. Supervision** | `OpenPort`'s wait (VQ28); deadlines on calls to servers; drivers' `STOP` (§11 D1); `init` (VQ29) |
| **8. Debts, and the boards** | §10.8's open findings, `KLog`'s ring, V5's carried items, the RV2's `dos` stall; the kill test 10 runs in a row |

Steps 1 to 3 are in order. Step 4 needs step 1, and is built on 2 and 3 as its first two libraries. Step 5 needs 2 and 3. Step 6 needs 2, 3 and 4. Step 7 is independent of 1 to 6, and step 8 needs 7.

## 1. The runtime floor

**Done (2026-10-22), on QEMU.** Every program links compiler-rt's 128-bit builtins, `mem*` by the vector unit, and `thread_local`, a copy per task.

- **compiler-rt** (§17.4.1): LLVM 22.1.8's `udivmodti4.c`, `udivti3.c`, `umodti3.c`, `divti3.c`, `modti3.c` and `multi3.c`, with the headers they include, unchanged in `third_party/compiler-rt/` with the licence and a README. They're built into `libcompiler-rt.a`, which goes last on every user link line, so a program takes only what it calls. Clang 22 inlines 128-bit multiplication on both ISAs, and combines a division and a modulo of the same operands into `__udivti3` and a multiply, so `int128` links two of the six.
- **`mem*` by words, then by SIMD.**
  - `lib/rt/string.c` moves 8 bytes a step: aligned heads, words (4 to a step), byte tails. Where source and destination are out of step, `memcpy` shifts aligned words together. Those reads may go up to 7 bytes past the source's end, but stay within the word that holds its last byte.
  - Each function is weak, and `arch/<isa>/user/string.S` replaces `memcpy`, `memmove` and `memset`.
  - **RVV:** e8 with m8, as many bytes a step as `vsetvli` grants.
  - **NEON:** 32 bytes a step through `q` registers. A length of 32 or more takes its last 32 bytes as one more overlapping step, so there's no byte tail. Shorter lengths are two overlapping loads of the largest size that fits.
  - `memcmp` stays the word loop on both ISAs.
  - Picked by the ISA at build time until step 4.
- **`CopyMem`, `CopyMemQuick`, `SetMem`**: inline in `planar/exec.h` over `memmove`, `memcpy` and `memset`, in Exec's order, source first. `CopyMem` is right for overlapping ranges, which Exec didn't promise.
- **`thread_local`.**
  - `user.ld` gathers `.tdata` and `.tbss`, aligned to 64, and the link fails for a program whose TLS segment is aligned to more. On Arm, the variables' offsets from the thread pointer depend on that alignment.
  - Each task's block is at the top of its own stack. No allocator is needed, and it goes with the task. crt0 makes the first task's (`lib/rt/tls.c`) and sets `tp` or `TPIDR_EL0`.
  - **Changed in the kernel:** `CreateTask` became a library function (`lib/exec/task.c`) over the kernel call, renamed `_CreateTask`. The call now takes a third argument and starts the task with it in the second argument register. CreateTask starts every task at crt0's `_task_start`, with the function in that register. The trampoline makes the block, calls the function, and calls `ExitTask(0)` if it returns. So a task's entry function may now return, which closes the autodoc's one bug.

**Tested by:**
- `conformance/int128.c`: 18 cases against the compiler's constant folding, unsigned, signed and `_BitInt(128)`, with each operand read back through a volatile. Then 4,096 random cases, with divisors of every width from 1 bit to 128, checked by `a = q·b + r`, `r < b`.
- `conformance/memfns.c`, running `conformance/memcheck.h`'s cases: every length from 0 to 512, at every source and destination alignment within 8, between guard bytes. `memcmp` equal and one byte apart, either way. `memmove` over its own buffer at every overlap to ±72 bytes. That's 257,768 cases.
- `make rt-test`: the same cases on the host, with the sanitizers, against `string.c`'s word loops renamed. It's part of `make test-all`. Mutation-tested: a wrong shift in the out-of-step copy fails at length 16.
- `conformance/tls.c`: two tasks each start with the first values and zeros, set their own, and find them kept after the other has set its. Each copy is in the Area its task's locals are in, and those differ. The second task ends by returning.
- `V6.1 runtime ok`, on every QEMU boot.

**Costs**, under QEMU, 64 KiB at a time:

| | RV64 (RVV) | AArch64 (NEON) |
|---|---|---|
| `memcpy` | 5.0 GB/s | 8.0 GB/s |
| `memset` | 6.5 GB/s | 9.7 GB/s |
| A byte loop | 0.66 GB/s | 0.41 GB/s |

QEMU's figures count instructions, not the hardware.

**Not yet on the boards.** The RV2's X60 has 256-bit vectors, twice QEMU's, and the CIX's cores have NEON. Each board runs `V6.1` the next time it's on the bench, with its own costs.

## 2. exec.library's memory and formatting

**Done (2026-10-22), on QEMU.** Exec's V39 pools, V36 `AllocVec` and `RawDoFmt`, in `lib/exec/pool.c` and `fmt.c`, built into `libexec.a`, which every link line has after the program and its libraries. A program that doesn't call them doesn't carry them.

- **Pools.**
  - Size classes: 16-byte steps to 256 bytes, then four to each doubling, so a block wastes at most a fifth of itself and has no header.
  - A free list per class, and a bump pointer in the current puddle. A block above the threshold is an Area, with a record on the pool's list, which is itself a small block.
  - The pool's header and lists are at the start of its first puddle, so `DeletePool` frees the puddles last.
  - `CreatePool` refuses a threshold whose class wouldn't fit in a puddle, and any flag but `AREAF_CLEAR`, `AREAF_CONTIG`, `AREAF_DMA32`, `AREAF_NC` and the new `POOLF_LOCKED` (Exec's `MEMF_SEM_PROTECTED`).
  - `POOLF_LOCKED` is a three-state futex lock on the header, `WaitAddr` and `WakeAddr` within the one process (§12.5 rules them out only between processes).
  - `CreatePoolMem`'s threshold is its buffer's size; it never allocates an Area.
- **`AllocVec`.** A 16-byte header with the size, from a process pool made at the first call (64 KiB puddles, a 16 KiB threshold, locked). Two tasks making it at once keep one, by compare-and-swap. `AREAF_DMA32` or `AREAF_NC` gives an Area of its own; `AREAF_EXEC`, `AREAF_LARGE` and the rest are refused.
- **`RawDoFmt`.** C's conversions but floating point's, with C's sizes, C23's `%b` and `%wN`/`%wfN`, and `%S` for a `struct Str` by value. `struct Str` and `STR()` are in `planar/exec.h` until step 3's `utility.h`. It returns the bytes before the NUL, where §17.4.2 had `void`.

**Tested by:**
- `conformance/pool.c`:
  - 256 blocks over 9 puddles, checked, freed and taken again with no new puddle.
  - Three blocks above the threshold, an Area each.
  - `AREAF_CLEAR` on a block taken again.
  - `DeletePool` with blocks outstanding, leaving the process's handle count (`QueryProcess(0)`) where it was before `CreatePool`.
  - A `CreatePoolMem` pool at an odd address, filled to its last block (489 of 16 bytes in 8 KiB), freed and filled again.
  - `AllocVec` at every size from 1 to 300 bytes, with and without `AREAF_CLEAR`, and larger ones; `AREAF_DMA32` below 4 GiB, an Area; the refusals.
  - Two tasks churning `AllocVec` and `FreeVec` at once, each checking its blocks' bytes.
  - `conformance/fmtcheck.h`'s 31 `RawDoFmt` cases.
- `make exec-test`: the same `RawDoFmt` cases on the host, with the sanitizers. Pools are churned 600,000 times over fake Areas, with thresholds of 1 KiB, 3,000 bytes and 16 bytes, every block's bytes checked before it's freed, and every Area gone after `DeletePool`. Also a fixed-buffer pool, and `AllocVec`'s reuse. It's part of `make test-all`. Mutation-tested: size classes one step too small fail it.
- `V6.2 exec memory ok`, on every QEMU boot.

**Costs**, under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `AllocPooled` and `FreePooled`, 64 bytes | 95 ns | 43 ns |
| `AllocVec` and `FreeVec`, through the lock | 204 ns | 78 ns |
| `RawDoFmt` of a short line | 1.3 µs | 0.66 µs |
| `pool.o`, `fmt.o` (text) | 1.6, 3.1 KiB | 2.1, 3.8 KiB |

**Not yet on the boards**, with step 1.

As §17.4.2 specifies:
- **Pools:** `CreatePool(flags, puddle, threshold)`, `CreatePoolMem`, `DeletePool`, `AllocPooled`, `FreePooled`. Puddles are Areas, allocations above the threshold are Areas of their own, and the flags carry `AREAF_*`, so a pool can be `AREAF_NC` or DMA32. `DeletePool` frees everything at once.
- **`AllocVec(size, flags)` / `FreeVec`:** a 16-byte header, from a default pool per process created at the first call.
- **`RawDoFmt`:** Exec's format string with C23's widths, plus `%S` for a `Str`. It writes through a callback, so it allocates nothing.
- **Tested by** `conformance/pool.c`: puddles filled and reused, allocations above the threshold, a fixed-buffer pool, `DeletePool` with allocations outstanding (the process's Area count back to where it was), `AllocVec` at every alignment, and `RawDoFmt` against known strings.

## 3. `utility.library`

As §17.4.3 specifies, in `include/planar/utility.h` and `lib/utility/`:
- **Tags:** `struct TagItem` with 64-bit data, `NextTagItem` (with `TAG_MORE`, `TAG_SKIP`, `TAG_IGNORE`), `FindTagItem`, `GetTagData`, `TagInArray`.
- **Hooks:** `struct Hook` and `CallHookPkt`.
- **Strings:** `Stricmp`, `Strnicmp`, `ToUpper`, `ToLower`, `SNPrintf` and `VSNPrintf` on `RawDoFmt`.
- **`Span` and `Str`:** a pointer and a length, with the Str helpers and the endian readers, each checked against the span's end with `stdckdint.h` and failing instead of reading past it. `font.library` (V8) parses every table this way.
- **`conformance/log.h`** moves onto `SNPrintf`, and its own formatting goes.
- **Tested by** `conformance/utility.c`, and the endian readers by a host fuzzer under ASan and UBSan (`tools/span-fuzz`), as §14's own fuzzer will be.

## 4. Interface tables and the shared library region

The design says three things (§3.4, §7.2, VQ12): shared libraries live in a global RX region bound once at boot, public libraries are reached through interface tables, and a process finds `ExecBase` somehow (VQ6, open). Nothing is built.

- **A library image** is a PIE ELF like a program's, in the boot bundle, with only code and read-only data: no writable section. The build refuses one that has one. What a library keeps per process lives in memory its caller gives it: a pool, a structure, or `thread_local` storage in the caller's block. Per-process library data is a decision (below).
- **The region.** At boot, the kernel relocates each library once into an RX Area and maps it into every domain as it's created, global (RISC-V's G bit, Arm's nG clear), at its physical address as everything is. A process's page count includes each library once. Mapping it into a new domain costs page-table entries, not memory.
- **`OpenLibrary(name, version)`** returns the library's base: its version and its interface tables. **`GetInterface(base, "main", version)`** returns a table: a `struct Interface` header (name, version, size), then function pointers. `IExec->AllocVec(…)` is a call through the table. `CloseLibrary` and `DropInterface` count users, though nothing is unloaded in V6. The tables are built once at boot, in the read-only part of the image.
- **How a process finds `IExec`** (VQ6): crt0 receives it in the second argument register, beside `main`'s argument in the first. Its leaning was the first register, but that one is `main`'s already. crt0 stores it in a global of the program's own, and `planar/exec.h`'s inline wrappers call through it.
- **SIMD through the table** (VQ22): `exec.library`'s `CopyMem` and `SetMem` entries are chosen when its table is built, by what the CPU has: RVV or a word loop on RISC-V, NEON, or SVE2 where present, on Arm. Step 1's build-time choice goes. The cores of a board all have the same vector ISA. Where they don't, the table uses the common subset.
- **Static linking stays** for `lib/rt`, `lib/drv` and anything private. Programs move onto `IExec` and `IUtility` as steps 5 and 6 touch them. The calls keep their names: `IExec->` is added, nothing is renamed (Libraries README).
- **Tested by** two processes calling `exec.library` and `utility.library` through their tables, checking the interface versions, a version too new refused, and their page counts showing one copy of each.

## 5. `lib/clib`

§17.4.5: `include/clib/` and `lib/clib/`, opt-in, never in a program that doesn't ask for it. `stdlib.h` (`malloc` and friends over `AllocVec`, `strto*`, `qsort`, `bsearch`, `abs`), `string.h`, `ctype.h`, `stdio.h`'s `snprintf` family only, and `stdbit.h`. acpid builds with `-Iinclude/clib` and `libclib.a`, and its `clib.c` and `heap.c` are deleted. Its tests (`make acpid-test`, the CIX's namespace) must give the same reports as before.

## 6. dos.library in RKM names

§17.4.4's table: `dos_open` → `OpenDos`, `dos_file` → `OpenFile`, `dos_read` → `ReadFile`, `dos_run` → `RunCommand`, `con_puts` → `PutStr` and so on. All at once, with no snake_case aliases left behind (a decision, below). Then the new calls:
- `ReadArgs(template, array, rdargs)` and `FreeArgs`, with AmigaDOS templates (`FILE/A,ALL/S,TO/K`);
- `FilePart`, `PathPart`, `AddPart`;
- `Printf`, `VPrintf`, `PutStr` on `RawDoFmt`.

Every command in `cmd/` moves to `ReadArgs`, and the Shell's script test (`SYS:EFI/planar/S/test`) gets lines that use the new templates. The autodocs and DOS manual are renamed with it.

## 7. Supervision

- **`OpenPort`'s wait** (VQ28): a flag and a deadline, so that `OpenPort` sleeps until a port of that name exists, or the deadline passes (`ERR_TIMEOUT`). The kernel wakes waiters on `CreatePort`. `lib/blk`'s and the tests' polling loops go. The leaning's other half, the supervisor holding a port across its server's restarts, is a decision (below).
- **Deadlines on calls.** `DoMsgUntil(port, msg, deadline)` in lib/exec: `ERR_TIMEOUT` when the reply hasn't come. The message is still the server's then. It's lost to the caller until the reply comes or the port goes, and the port says so (`mp_Lost`). dos.library, the power votes, and the class libraries' calls use it, with deadlines each class states. A server that misses one is reported by its client, and hwdb is told the server is unresponsive.
- **`STOP`** (§11 D1): `HWDB_STOP` with a deadline, and drv.library's `stop` callback. A driver turns its device off, gives back its DMA and replies. hwdb then revokes its lease without quarantine. If the deadline passes, it kills the driver, as now. hwdb stops every driver in reverse graph order when it stops, and the kill test stops some drivers instead of killing them, then checks their leases were idle.
- **`init`** (VQ29): `servers/init`, a small program the kernel starts as the root instead of hwdb. It starts hwdb and waits on it. When hwdb dies, `init` logs a meditation (its exit, its fault and the last lines of its log) and lets the board's watchdog reset it, or resets it by PSCI or SBI where there's none. In test mode it reports hwdb's exit as the kernel does now. hwdb keeps supervising everything below it.

## 8. Debts, and the boards

- **§10.8's open findings:**
  - `time_to_us` overflowing (`kernel/ktime.c`);
  - AArch64 domain creation failing when 8-bit ASIDs run out (`arch/aarch64/mmu.c`: rollover);
  - no AArch64 kernel stack guard;
  - `SCTLR_EL1.nTWE` and `DZE` clear, so a `wfe` or `dc zva` in user mode kills the process;
  - `CNTKCTL` ORed, not set;
  - the PSCI call missing its x4–x17 clobbers;
  - port names first come, first served.

  Each is fixed with a test that fails without the fix, or closed with the reason. The findings the review's digest didn't recheck are rechecked, and each one is fixed or listed.
- **`KLog`** (M12): a ring the console drains, so that a log line costs a copy, not a spin with interrupts off.
- **From V5:** a sync object woken only when someone sleeps on it (a flag in `struct GpuSync`), which saves the CIX's server an interrupt and six calls a job when the client spins; a GPU fault reaching the client as a message, not only `GPU_STATUS`.
- **The RV2's `dos` stall** (v5-plan §7): the last `gpu-rogue` that the kill rounds start waits forever on a `dos` call, in 2 runs of 4. Step 7's deadline turns the hang into an error. The cause is still to find. First a mark of what `dos`'s server task is doing, kept with no extra task, since a watchdog task seemed to hide the stall.
- **The exit's kill test:** 10 runs in a row on each board, with the transcripts kept.

## Sources

- §17 (the runtime and libraries), §7.2 and VQ12 (interface tables), §3.4 (the shared library region), §11 (D1's `STOP`), §12.6 and §12.7 (VQ28, VQ29), §10.8 and §10.9 (the review).
- The RKM's *Libraries* (3rd edition) and *Includes and Autodocs*, for Exec's, utility's and dos's calls, and the AmigaOS 4 SDK's interface conventions for `GetInterface`.
- LLVM 22's `compiler-rt/lib/builtins`.

## Decisions

1. **The shared region in V6.** Build it now, or link the interface tables statically and share later. *Leaning:* now (the roadmap's leaning). §14's atlas and §16's bitmaps assume libraries mapped once at one address, and the region is VQ6's natural home.
2. **A library's per-process data.** None: libraries are code and constants, and state lives in what the caller passes. Or a per-process data block the loader allocates for each library a process opens, found through the interface. *Leaning:* none in V6. Every library step 4 builds can work that way, and it keeps the region read-only.
3. **How a process finds `IExec`** (VQ6). The second argument register at entry, or a fixed address in the shared region, which the identity layout can't guarantee. *Leaning:* the register.
4. **SIMD choice** (VQ22). Once per CPU at boot, or through the interface table. *Leaning:* through the table, built at boot.
5. **The dos rename.** At once, or with snake_case aliases for a phase. *Leaning:* at once. Every caller is in this tree.
6. **Ports across a restart** (VQ28). `OpenPort`'s wait alone, or also a supervisor holding each supervised server's port so its name never goes. *Leaning:* the wait in V6, and supervisor-held ports when a server's clients can't tolerate the window.
7. **hwdb's death** (VQ29). A meditation and a reset, or restarting hwdb, which would have to adopt the leases and units of a dead one. *Leaning:* the reset, as VQ29 leans. Restarting in place is a phase of its own.
