# V6 plan: foundations, libraries and supervision

**Status: in progress (2026-10-22): steps 1 to 7 done, step 8 done but for the RV2; decisions 1 to 7 taken as they leaned.** Scoped from the [roadmap's replan after V5](08-roadmap.md#replanned-after-v5): §17 (the C runtime and libraries), §7.2 and §3.4 (interface tables and the shared library region), §11 and §12 (the driver model's and let-it-crash's unbuilt halves), §10.8 (the review's open low findings), and what V5 carried forward. The decisions at the end are all taken.

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

**Done (2026-10-22), on QEMU.** In `include/planar/utility.h` and `lib/utility/` (`tags.c`, `string.c`, `str.c`, `span.c`), built into `libutility.a`, which every link line has before `libexec.a`, since `SNPrintf` calls `RawDoFmt`.

- **Tags and hooks** as Commodore's, with 64-bit data. `NextTagItem` leaves `*tagListPtr` `nullptr` at the end, so a finished walk stays finished; a `TAG_MORE` to `nullptr` ends the list. `CallHookPkt` calls `h_Entry` as a C function: no register stub.
- **Case is ASCII's**, not ISO 8859-1's as Commodore's was, because Latin-1's mapping would change UTF-8's lead bytes. `Stricmp` compares the bytes as unsigned.
- **`SNPrintf` and `VSNPrintf`** return what C's do: the whole length, so a result of the size or more says the text was cut. The buffer always ends with a NUL if its size isn't 0.
- **`Str`** helpers return views, nothing copied. `StrSlice` clamps; `StrToU64` reads the whole `Str` or fails, refusing overflow by `ckd_mul` and `ckd_add`.
- **`Span`'s readers** check `offset + n` with `ckd_add` before forming a pointer, and leave `*out` alone when they refuse. `SpanReadU8` is added to §17.4.3's list.
- **`struct Str` and `STR()`** moved from `planar/exec.h`.
- **`conformance/log.h`** is now debug.lib's `KPrintF`, over `VSNPrintf`, format-checked by the compiler. Its `log_str`/`log_num` are gone, and the 22 conformance programs that used them log through it. Every line keeps its text.

**Tested by:**
- `conformance/utilcheck.h`, 335 cases, run in the guest by `conformance/utility.c` and on the host with the sanitizers by `make utility-test`:
  - a tag list through `TAG_IGNORE`, `TAG_SKIP` and `TAG_MORE`, with items after each that mustn't be seen;
  - hooks, case for all 256 bytes, the comparisons' signs;
  - `SNPrintf` fitting, cut, and with sizes 1 and 0;
  - every `Str` helper's edges, and `StrToU64` at `UINT64_MAX` and one past it;
  - every reader at the end, one past it, and at offsets that would wrap a 64-bit add.
- `tools/span-fuzz` (`make span-fuzz`): libFuzzer with ASan and UBSan over the readers, `SpanSlice`, `StrToU64`, `StrSplit` and `StrTrim`, against a model whose bounds are 128-bit. Each input's bytes are an exact-size block, so a read one past is ASan's. A million inputs in `make test-all`, and 61 million in a minute's run with no failure. Mutation-tested: the check as a plain add, and one byte too far, each fail it within a second, and `utility-test` too.
- `V6.3 utility ok`, on every QEMU boot.

**Costs**, under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `GetTagData` of the eighth tag | 50 ns | 55 ns |
| `SNPrintf` of a short line | 1.2 µs | 0.75 µs |
| `SpanReadU32BE` | 40 ns | 15 ns |
| `lib/utility`, all four files (text) | 1.8 KiB | 2.4 KiB |

**Not yet on the boards**, with steps 1 and 2.

As §17.4.3 specifies, in `include/planar/utility.h` and `lib/utility/`:
- **Tags:** `struct TagItem` with 64-bit data, `NextTagItem` (with `TAG_MORE`, `TAG_SKIP`, `TAG_IGNORE`), `FindTagItem`, `GetTagData`, `TagInArray`.
- **Hooks:** `struct Hook` and `CallHookPkt`.
- **Strings:** `Stricmp`, `Strnicmp`, `ToUpper`, `ToLower`, `SNPrintf` and `VSNPrintf` on `RawDoFmt`.
- **`Span` and `Str`:** a pointer and a length, with the Str helpers and the endian readers, each checked against the span's end with `stdckdint.h` and failing instead of reading past it. `font.library` (V8) parses every table this way.
- **`conformance/log.h`** moves onto `SNPrintf`, and its own formatting goes.
- **Tested by** `conformance/utility.c`, and the endian readers by a host fuzzer under ASan and UBSan (`tools/span-fuzz`), as §14's own fuzzer will be.

## 4. Interface tables and the shared library region

**Done (2026-10-22), on QEMU.** Decisions 1 to 4 went as they leaned, but for one register (below).

- **Library images** (`libs/exec`, `libs/utility`): PIE images of code and constants with a `PLIB` header (`lib/libhead.S`, `planar/libimage.h`), linked by `arch/<isa>/user/lib.ld`. Its `ASSERT` refuses a library with data or `.bss`: tried with a counter added to utility.library. Pointers relocated at boot go with the constants (`.data.rel.ro`, `.got`), since nothing runs before the kernel has written them. Built for the ISA's base, without V, so one image runs on any hart. exec.library is 8.3 KiB and utility.library 6.7 KiB on RV64.
- **The region** (`kernel/library.c`): one Area that the kernel fills at boot, before the first process. Each image goes in a page apart and is relocated there once (`R_*_RELATIVE` only, every offset checked). Then its alternatives are applied, and exec.library's directory gets every library's base. Every process maps the whole region read and execute with `MAP_GLOBAL` (RISC-V's G bit, Arm's nG clear) as it's created (`library_map`). The region is never freed, and is outside the leak checks' Area count. It's 5 pages on RV64 and 7 on AArch64.
- **`OpenLibrary`, `GetInterface`, `CloseLibrary`, `DropInterface`** are exec.library's, in its main interface. `struct Library` has a name, version, revision, ID string and interfaces, and `struct Interface` a name, version, size and library. `CloseLibrary` and `DropInterface` count nothing: the region is read-only and nothing is unloaded, so the plan's count has nowhere to live. Both libraries are version 6.4, with main interfaces at version 1.
- **`IExec`** (decision 3): the third argument register (`a2`, `x2`), not the second. The second is already hwdb's root lease (`process_create_root`). crt0 stores it in the program's `IExec` (`lib/rt/tls.c`) after relocating.
- **Per-process data** (decision 2): none in the library. What exec.library needs per process (`AllocVec`'s pool and crt0's `_task_start` for `CreateTask`) is a `struct ProcessData` in the program (`lib/exec/procdata.h`). The first word of every task's thread block points at it (`lib/rt/tls.c`). RISC-V's crt0 now puts tp 64 bytes into the block, as Arm's TCB already was. The linked-in `AllocVec` and the library's share the pool.
- **SIMD through the table** (decision 4): exec's `CopyMem`, `CopyMemQuick` and `SetMem` start as word loops, and the kernel repoints them at `string.S`'s, renamed, where `arch_has_vector()` says so: RVV, or NEON (always). SVE2 isn't used, since the kernel doesn't save SVE's registers. The static `mem*` that programs link stay the build's choice.
- **`ProcessInfo`** gains `pi_Pages` and `pi_LibraryPages`.
- **utility.library** has its own copy of `fmt.c` for `SNPrintf`: a library can't keep exec's interface to call through.

**Tested by:**
- `conformance/library.c`:
  - `IExec` and its header.
  - `OpenLibrary` and `GetInterface`: the right base and interfaces, with too new a version, an unknown name and an unknown interface refused.
  - `CopyMem` and `SetMem` through the table at every length to 300 and 8 alignments, and overlapping.
  - `AllocVec` freed by the other `FreeVec` both ways, and `CreateTask` through the library starting a task with its own `thread_local`.
  - `IUtility`'s `SNPrintf`, `GetTagData`, a Span read and a refused one.
  - A second process: the same `pi_LibraryPages`, `QueryArea` of `IExec` showing the kernel's Area, read and execute, two or more sharers and the region's size, the same calls, and `IExec`'s address back as its exit code, equal to the first's.
  - A third process, which writes to the table and faults with `PFAULT_WRITE` at that address.
- `V6.4 libraries ok`, on every QEMU boot. It says how many libraries, how many pages and whether `CopyMem` is the vector unit's.

**Costs**, under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `AllocVec` and `FreeVec`, linked in | 190–210 ns | 80–97 ns |
| The same through `IExec` | 195–205 ns | 83–92 ns |
| An empty `GetTagData` through `IUtility` | 25 ns | 10 ns |
| The region | 5 pages | 7 pages |

**Not tested:**
- A RISC-V hart without V, for the word-loop path. QEMU's hart has V, and the programs themselves are built with it.
- That the TLB keeps one entry per page: the G bit is set, but nothing measures it.

**Not yet on the boards**, with steps 1 to 3.

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

**Done (2026-10-22), on QEMU.** `include/clib/` (`assert.h`, `ctype.h`, `errno.h`, `inttypes.h`, `stdbit.h`, `stdio.h`, `stdlib.h`, `string.h`) and `lib/clib/`, built into `libclib.a`, which a program's link line has before `libutility.a` only when it asks: acpid, and conformance programs named `clib*`.

- **acpid** builds ACPICA and its own files with `-Iinclude/clib`. `acplanar.h` includes `<ctype.h>`, `<stdlib.h>` and `<string.h>` instead of declaring the functions, on Planar and the host alike. `AcpiOsAllocate` is `malloc`, so `acpid_alloc` is gone from `acpid.h` and `host.c`, and `clib.c` and `heap.c` are deleted. ACPICA keeps its own `snprintf` (`utprint.c`). The image is 4 KiB bigger on AArch64 and 8 bytes on RV64.
- **The reports are the same:** `hwdb-test`'s acpid on the CIX's tables (271 devices, 217 present) and QEMU's (50), and acpid on the QEMU ACPI boot (53 devices, 53 present). Planar's acpid on the CIX's own tables waits for the board.
- **`malloc`** is `AllocVec`; `realloc` reads a block's size through `_vec_size` (`lib/exec/vec.h`), lib/exec's, not exec.library's. `malloc(0)` is a block, `realloc` keeps a block big enough, `qsort` is a heapsort, `errno` is `thread_local`, the `strtol` family takes C23's `0b`.
- **Not as §17.4.5 said:** `<stdbit.h>` is on `__builtin_clzg`, `_ctzg` and `_popcountg`, since Clang 22 has no `__builtin_stdc_*`; `memexplicit` is C23's `memset_explicit`. lib/clib calls the linked-in `AllocVec` and `VSNPrintf`, not `IExec`'s: the same pool either way.
- **A linker fix:** `errno` was the first `thread_local` with no first value in a program with no other, and the linker dropped the empty `.tdata` with its alignment. The TLS segment came out aligned to 4, which on Arm would have put the variables where crt0 doesn't; `mk/user.mk`'s check refused the link. `user.ld` aligns `.tbss` to 64 too.

**Tested by:**
- `conformance/clibcheck.h`, 6220 cases: the string functions at their edges, ctype for every byte and EOF, 29 `strtol` cases at the prefixes and the ranges' ends, malloc's alignment, `realloc` grown a byte at a time and past the pool's threshold, `qsort` and `bsearch` on 0 to 300 elements of 1, 4 and 24 bytes, `snprintf` and the `PRI` macros, `stdbit.h` for every byte against loops.
- `conformance/clib.c` runs them in the guest, then shows `errno` is each task's own and a block one task mallocs another frees. `V6.5 clib ok`, on every QEMU boot.
- `make clib-test` runs them on the host with the sanitizers, then 900,000 checks against the host's library: random strings through both `strtol` families, the string functions, and `qsort`. It found `strstr` wrong for an empty needle. Mutations caught: the overflow check gone, the heap's sift one past the end, `strncpy` not padding, `realloc` keeping a block 16 bytes too small, and copying a byte short.

**Costs**, under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `malloc` and `free` of 64 bytes | 280 ns | 110 ns |
| `strtoul` of 10 digits | 90 ns | 85 ns |
| `qsort` of 1000 `int`s | 1.0 ms | 0.29 ms |

## 6. dos.library in RKM names

§17.4.4's table: `dos_open` → `OpenDos`, `dos_file` → `OpenFile`, `dos_read` → `ReadFile`, `dos_run` → `RunCommand`, `con_puts` → `PutStr` and so on. All at once, with no snake_case aliases left behind (a decision, below). Then the new calls:
- `ReadArgs(template, array, rdargs)` and `FreeArgs`, with AmigaDOS templates (`FILE/A,ALL/S,TO/K`);
- `FilePart`, `PathPart`, `AddPart`;
- `Printf`, `VPrintf`, `PutStr` on `RawDoFmt`.

Every command in `cmd/` moves to `ReadArgs`, and the Shell's script test (`SYS:EFI/planar/S/test`) gets lines that use the new templates. The autodocs and DOS manual are renamed with it.

**Done (2026-10-22), on QEMU.** §17.4.4's names, all at once (decision 5): `struct Dos` and `struct Con`, `OpenDos`, `OpenFile`, `ReadFile`, `ExaminePath`, `ExamineNext`, `DeletePath`, `CreateDir`, `NextVolume`, `RunCommand`, `SplitArgs`, `OpenCon`, `WriteCon`, `PutStr`. No snake_case name is left in `planar/dos.h` or a caller: the commands, the Shell, dostest, hwdb and the GPU drivers' firmware loaders. lib/dos is an archive now, `libdos.a`, of five files (the client, the loader, the console, the paths, the arguments), so a command carries only what it calls: Echo's code is 22 KiB, without the loader.

- **`ReadArgs`** takes V36's templates: `/A`, `/K`, `/S`, `/N`, `/M` (giving back from its end what the `/A` items after it need), `/F`, aliases with `=`, keywords without case, `KEY=value`, quotes with AmigaDOS's `*` escapes. What's not given is left, so defaults are the caller's. The words are copied into one `AllocVec` block that `FreeArgs` gives back.
- **Not as AmigaDOS:** there's no `Input()` or `IoErr()`, so the line is named (`rda_Source`), the `struct RDArgs` is the caller's, and the error is returned with its reason in `rda_Why` ("FILE is required"). No `/T`. `?` gives an empty reason, for the caller to show the template; there's nothing to read a new line from.
- **`AddPart`** in place, with a volume in the name replacing the path, `:` the volume's root, and each leading `/` a step up; `FilePart` and `PathPart` as the RKM's. `dos_path` is gone: a command copies its directory and adds the name, and the Shell's `CD /` is `AddPart`'s.
- **`Printf` and `VPrintf`** over `RawDoFmt`, gathered 64 bytes a console message. `con_dec` is gone.
- **The commands' templates:** `Echo TEXT/M,NOLINE/S`, `Type FROM/A/M,NUMBER/S`, `List DIR/M,FILES/S,DIRS/S,QUICK/S`, `Info VOLUME`, `Touch FILE/M/A`, `Delete FILE/M/A,QUIET/S`, the Shell's `FROM` (was `-s`) and `CD DIR`. Each says why its arguments are wrong, with its template. A Shell started from a Shell begins in its directory.

**Tested by:**
- `conformance/argscheck.h`, 100 cases: the plan's template, every modifier, `/M` giving back to one and two `/A` items and not enough, `/F` from a positional and a keyword and over a 600-byte word, quotes and escapes, `=` with empty and quoted values, aliases, numbers at `INT64_MIN` and `INT64_MAX` and one past each, every reason, the template's own errors (32 items, 33, `/T`, two `/M`), defaults left, `FreeArgs` twice. `AddPart` at its volumes, steps up and room to the byte; `FilePart`, `PathPart`.
- `conformance/dosargs.c` runs them in the guest: `V6.6 dos ok`, on every QEMU boot.
- `make dos-lib-test` runs them on the host with the sanitizers, with `AllocVec` the host's `malloc` and the words last in the block, so a copy past it is the sanitizer's to see. Then 200,000 random lines of quotes, stars, equals signs and keywords against seven templates (120,000 parse; each value must lie in the block), and `Printf` and the console over a fake `con.0` that records each message: what's sent and in how many, a console that goes and is found again once, and none, to the log. 12 mutations caught (one, reconnecting to a console without end, by hanging), among them the block's room for `/F`'s second copy, which first got through: the overrun landed in the numbers' part of the block, until the words went last.
- The Shell's script, on every QEMU boot with a disk: `List DIR=… QUICK FILES`, `CD C`, `CD /`, `Type FROM=… … NUMBER`, a quoted `Echo` with `NOLINE`, and the lines it had.

**Costs**, under QEMU: `ReadArgs` and `FreeArgs` of a four-word line with a keyword, a switch and a quoted word, 1.8 µs on AArch64 and 3.4 µs on RV64, most of it `AllocVec`'s.

## 7. Supervision

- **`OpenPort`'s wait** (VQ28): a flag and a deadline, so that `OpenPort` sleeps until a port of that name exists, or the deadline passes (`ERR_TIMEOUT`). The kernel wakes waiters on `CreatePort`. `lib/blk`'s and the tests' polling loops go. The leaning's other half, the supervisor holding a port across its server's restarts, is a decision (below).
- **Deadlines on calls.** `DoMsgUntil(port, msg, deadline)` in lib/exec: `ERR_TIMEOUT` when the reply hasn't come. The message is still the server's then. It's lost to the caller until the reply comes or the port goes, and the port says so (`mp_Lost`). dos.library, the power votes, and the class libraries' calls use it, with deadlines each class states. A server that misses one is reported by its client, and hwdb is told the server is unresponsive.
- **`STOP`** (§11 D1): `HWDB_STOP` with a deadline, and drv.library's `stop` callback. A driver turns its device off, gives back its DMA and replies. hwdb then revokes its lease without quarantine. If the deadline passes, it kills the driver, as now. hwdb stops every driver in reverse graph order when it stops, and the kill test stops some drivers instead of killing them, then checks their leases were idle.
- **`init`** (VQ29): `servers/init`, a small program the kernel starts as the root instead of hwdb. It starts hwdb and waits on it. When hwdb dies, `init` logs a meditation (its exit, its fault and the last lines of its log) and lets the board's watchdog reset it, or resets it by PSCI or SBI where there's none. In test mode it reports hwdb's exit as the kernel does now. hwdb keeps supervising everything below it.

**Done (2026-10-22), on QEMU.** Decisions 6 and 7 as they leaned: `OpenPort`'s wait alone, and a meditation and a reset for hwdb's death.

- **Kernel.** `task_wait_until` (`kernel/sched.c`), a wait with a deadline on the kernel's own timer, under three calls: `WaitUntil(mask, deadline)`, new (and exec.library's interface version 2); `SignalWaitConn`'s new deadline; and `OpenPort`'s sixth argument, `deadline`, which lists the caller under the port lock until `CreatePort` makes the name (`SIGF_PORT`, a kernel signal). One convention for all: `CurrentTime`'s clock, `DEADLINE_NONE` never, a time already past (0 among them) not waiting. `GrantRoot` gives a held child the device root, and `ColdReboot` resets the board for the root's holder.
- **Not as planned:** `OpenPort` has a deadline and no separate flag: 0 is "don't wait", which keeps every old caller's meaning. `WaitUntil` wasn't in the plan; `DoMsgUntil` needs a wait with a deadline that leaves the task's one `ArmTimer` alarm alone.
- **lib/exec.** `ConnectPortUntil`; `DoMsgUntil`, with the message given up on kept in `mp_Lost` and its late reply dropped wherever it's taken, and `ERR_BUSY` until then; `DoMsg` is `DoMsgUntil` with no deadline. `ReportUnresponsive(port)` (`report.c`, in `libexec.a`) sends hwdb `HWDB_UNRESPONSIVE`.
- **The polling went:** lib/blk's connect loop, the Shell's and dostest's wait for dos (`OpenDosUntil`), gputest, inputtest, displaytest, the kill test's clients, and the conformance clients' spins.
- **Deadlines:** dos.library `DOS_DEADLINE_MS` (10 s), lib/blk `BLK_IO_MS` (5 s), the console `CON_DEADLINE_MS` (1 s), xhci's vote and hwdb's own votes `POWER_DEADLINE_MS` (2 s), drv.library's and acpid's calls to hwdb `DRV_HWDB_MS` (5 s). Each but hwdb's reports the server that misses it, and hwdb restarts it as a death it asked for, once a second at most (a vote hwdb waited for too long restarts power). The GPU and display classes have no client library yet: their clients are the conformance programs, which keep their own.
- **`STOP`.** drv.library leaves an `HWDB_STOP` with hwdb after `HWDB_STARTED`; hwdb answers it to ask the driver to stop, by `DRV_STOP_MS` (1 s). The library calls the driver's new `stop` callback for each device and exits 0, and one that hasn't exited by then is killed. virtio-mmio's `stop` resets the device and frees its queue's Area. When hwdb stops (the kill test's end, `EndShell` with `reboot`) it asks each unit once no running unit depends on it, by a reference or a parent: reverse graph order.
- **`init`.** `servers/init`: started by the kernel with the root, it starts hwdb held, gives it the root and waits. hwdb's death is a meditation (the fault, address and PC, or the exit code) and a `ColdReboot`; in test mode, a report. The kernel's two hwdb boots, the test and the system, start init now. hwdb's last log lines aren't in the meditation: they wait for `KLog`'s ring (step 8).

**Tested by:**
- `conformance/deadline.c` (`V6.7 supervision`, every QEMU boot): `OpenPort` at deadline 0, one 50 ms on and one past; a client waiting from before its server's `CreatePort` connecting when it's made; `WaitUntil`'s deadline, a signal there already, and its polls; `DoMsgUntil` in time, too late, `ERR_BUSY` twice, the late answer dropped and nothing stashed, and `ERR_PORT_DIED` once a server holding a lost message dies; a task waiting in `OpenPort` for ever killed, gone in 115–280 µs.
- The same step starts init with `INIT_DEATH`: hwdb faults at once, init logs the meditation, says it would reset, and exits 0, leaving no process or lease. The V4.3 hwdb test and `BOOTARGS=shell` run through init, and `EndShell` with `reboot` stops every driver, ends init and resets the board under QEMU.
- hwdb's kill test: `V6.7 stop` asks the rng and rtc units alone to stop under their clients, and each exits 0 in time, with nothing quarantined and its lease idle, and starts again; `V6.7 report` has `conformance/reporter.c` say a running driver's port missed a deadline, and hwdb restarts that unit, once; `V6.7 stop all` asks all 7 units in reverse graph order at the end, none killed late, and checks as it asks each that nothing depending on it still runs (on QEMU's AArch64 boot, 4 such dependencies: pci's functions, and power's consumers). A mutation that asks every unit at once is caught; a first check, made at each death, wasn't, since units asked together exit together and are reaped in index order.
- `make dos-lib-test` adds a console that misses its deadline: reported, reconnected once, then the log.

**Costs**, under QEMU: a round trip to another process by `DoMsgUntil` about 24 µs, against `DoMsg`'s 18 µs, on both ISAs: the kernel arms and stops its timer around each wait.

## 8. Debts, and the boards

**In progress (2026-10-22): QEMU's and the CIX's half done; the RV2's to come** (`a9b82fc`, `7c20ed3`).

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

**Built:**
- **§10.8:** every low finding fixed or closed with its reason, in its row of [§10.8](10-code-review.md#108-low). The plan's seven:
  - `time_to_us` and `time_us` take seconds and the rest apart;
  - past the last ASID, domains share one, which a CPU flushes when it loads a different domain with it (both ISAs: RV64 failed the same way);
  - a kernel stack canary, `struct task`'s last word, checked at every switch. The stacks are in the direct map's blocks, so there's no guard page;
  - `nTWE` and `DZE` set; `CNTKCTL` and `MDSCR_EL1` written whole; `VPIDR_EL2` and `VMPIDR_EL2` the CPU's own; the PSCI call's x1–x17;
  - port names: naming a port is a right, `PROCF_NAMES`. The kernel's processes have it, a built-in program has it if its parent does, and a program loaded from a disk only if its parent gives it.
- **The rest, rechecked:**
  - fixed: RV64's SUM and `sscratch`; a kernel copy's fault retried on both ISAs; pmm's scan by runs; `task_set_affinity`'s refusal; ShareArea's I-cache sync; the counters; `AcceptPort` past a broken client; `ClosePort` from a sibling; memory nodes and wrapping ranges;
  - closed with the reason: SGI 0, never enabled; the woken task's affinity, which only a selftest can change; the lent priority, now bounded by the server's next wait; the hosted build's purpose; ProtectArea's sync ordering;
  - and `ktimer_stop`, found fixed already (`268591d`).
- **`KLog`'s ring:** `kprintf` and `KLog` copy into 64 KiB that a kernel task, `klog`, drains. It writes only what the UART takes without waiting (`console_room`: the PL011's FIFO flag, the 16550's 16 bytes once empty), so interrupts are off for microseconds. It's woken by an IPI to its own CPU, so a `kprintf` under any lock is safe. Before it runs, when kernel text finds the ring full, and before a halt or a reboot (`klog_flush`), the caller drains the ring itself as before. A process's line that finds it full waits for room.
- **`ReadLog`** (call 57): the root's holder reads the ring's tail. init's meditation ends with hwdb's last lines.
- **From V5:**
  - `GPU_WAIT`: the plan's flag in `struct GpuSync` couldn't save the interrupt, since the firmware can't read it. Instead the client says it will sleep, with the value it waits for, and `gpu-panthor` masks the group's `CSG_SYNC_UPDATE` in `ack_irq_mask` until then.
  - `GPU_EVENT`: left with the server and answered when the queue faults. `gpu-rogue` has both, `GPU_WAIT` doing nothing there; its `GPU_EVENT` waits for the RV2.
- **Found on the CIX:** hwdb's kill test failed "its DMA Areas weren't all quarantined": `gpu-panthor` boots its firmware after Bind, allocating as it goes, so the count taken before a kill can be short. It's now "at least what it held when counted".

**Tests,** each failing without its fix (by a mutation run, except where QEMU can't show it: the I-cache, `sscratch`, SUM, the system registers, PSCI, the counters):
- kernel: `V1.1 pmm` (one run longer than the largest: 1030 steps for 65,536 pages, against a step a page), `V1.1 timer` (a year both ways), `V1.3 stack` (a task recursing into its own struct, found at the switch), `V1.3 migrate`'s refusal, `V1.4 asids` (four domains, one ASID of their own, three sharing, each seeing only its own page at one address), `V2.4 klog` (a `KLog` while CPU 1 holds the console 50 ms returns in 38–61 µs under QEMU, 0 µs on the CIX);
- programs: `userinsn`, `copyrace` (20,000 `QueryArea`s into an Area being remapped), `crowd` (`V6.8 debts`), `deadline`'s sibling close, dostest's `C:hello` refused `"dos"`, init's test reading hwdb's last line back;
- the CIX: `gputest` fails if a spun-on job costs an interrupt, and its fault job must come back as `GPU_EVENT`'s answer.

**Costs**, on the CIX:
- a spun-on job: 8 µs, 0.0 interrupts and 0.1 kernel calls machine-wide, against V5's 11 µs, 1.0 and 6.0;
- a job slept on, `GPU_WAIT` included: 21–23 µs;
- a fault's message: about 1 ms after the doorbell;
- the failing scan for a run longer than any: 2.1 million steps, a word each of the holes between RAM's ranges.

**Still to do:** the RV2's half: the `dos` stall, `gpu-rogue`'s `GPU_EVENT`, and its 10 runs.

## Sources

- §17 (the runtime and libraries), §7.2 and VQ12 (interface tables), §3.4 (the shared library region), §11 (D1's `STOP`), §12.6 and §12.7 (VQ28, VQ29), §10.8 and §10.9 (the review).
- The RKM's *Libraries* (3rd edition) and *Includes and Autodocs*, for Exec's, utility's and dos's calls, and the AmigaOS 4 SDK's interface conventions for `GetInterface`.
- LLVM 22's `compiler-rt/lib/builtins`.

## Decisions

1. **The shared region in V6.** *Taken (2026-10-22): now* (step 4). Build it now, or link the interface tables statically and share later. *Leaning:* now (the roadmap's leaning). §14's atlas and §16's bitmaps assume libraries mapped once at one address, and the region is VQ6's natural home.
2. **A library's per-process data.** *Taken (2026-10-22): none in the library;* the program's `ProcessData`, through the thread pointer (step 4). None: libraries are code and constants, and state lives in what the caller passes. Or a per-process data block the loader allocates for each library a process opens, found through the interface. *Leaning:* none in V6. Every library step 4 builds can work that way, and it keeps the region read-only.
3. **How a process finds `IExec`** (VQ6). *Taken (2026-10-22): a register,* the third, since hwdb's root lease has the second (step 4). The second argument register at entry, or a fixed address in the shared region, which the identity layout can't guarantee. *Leaning:* the register.
4. **SIMD choice** (VQ22). *Taken (2026-10-22): through the table, at boot* (step 4). Once per CPU at boot, or through the interface table. *Leaning:* through the table, built at boot.
5. **The dos rename.** *Taken (2026-10-22): at once* (step 6). At once, or with snake_case aliases for a phase. *Leaning:* at once. Every caller is in this tree.
6. **Ports across a restart** (VQ28). *Taken (2026-10-22): the wait alone* (step 7). `OpenPort`'s wait alone, or also a supervisor holding each supervised server's port so its name never goes. *Leaning:* the wait in V6, and supervisor-held ports when a server's clients can't tolerate the window.
7. **hwdb's death** (VQ29). *Taken (2026-10-22): the meditation and the reset* (step 7). A meditation and a reset, or restarting hwdb, which would have to adopt the leases and units of a dead one. *Leaning:* the reset, as VQ29 leans. Restarting in place is a phase of its own.
