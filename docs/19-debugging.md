# 19. Debugging

**Status: proposed (2026-10-26).** Builds on the single address space ([§3](03-memory.md)), ports and messages ([§4](04-ipc.md)), the toolchain ([§7](07-language-toolchain.md)), "let it crash" ([§12](12-let-it-crash.md)) and the libraries ([§17](17-c-runtime-and-libraries.md)). The formats and the compiler's side are planar-c's [`docs/debug-info-design.md`](../../../lang/planar-c/docs/debug-info-design.md).

Planar has no debugger today. When a process faults, the kernel prints one line (`process_fault`: name, fault, address, PC), kills the process, and its supervisor restarts it. This document proposes how to get from that line to a source-level debugger, on the board and from a host, without DWARF or gdb.

## 19.1 Goals

1. **Every crash explains itself.** A fault's log names the image, the build and a full backtrace, and turns into file:line:column on the host or on the board. Post-mortem comes first: with supervision, most bugs are seen as a crash and a restart, not in a live session.
2. **One debugger, two places.** The same debugger core runs on the board, as a Shell command and later a Workbench window, and on a host connected by serial. They speak the same messages.
3. **Nothing borrowed that doesn't fit.** The compiler, the formats and the debugger are designed together (planar-c §2 says what was taken from DWARF's successors and why). gdb's remote protocol appears only as a wire protocol to QEMU's gdbstub (§19.9).
4. **Small enough to hold in your head.** The kernel's part is a few hundred lines: images, a debug port and its messages, and breakpoint and step support.

## 19.2 Prior art

- **The Amiga.**
  - SegTracker recorded where each loaded segment went, so any address could be named. §19.4 is the same idea, made authoritative.
  - Enforcer turned stray accesses into reports with the PC and the segment.
  - Wack and ROMWack were small monitors that were always there.
- **Mach exception ports and Fuchsia exception channels.** A fault is a message to whoever holds the right port, and the faulting thread waits for the answer. §19.6 does this with Exec ports.
- **Fuchsia's symbolizer markup and debuginfod.** The target logs build-ids, load addresses and PCs. Debug files are found by build-id wherever they are, and symbolization happens there. §19.5 does this.
- **Linux uprobes.** Breakpoints in code that other processes share are handled by executing the displaced instruction out of line in a per-task slot (XOL). §19.7 needs this for the library region.
- **rr and Pernosco.** Record and replay, and omniscient debugging. §19.10 explains why not yet.

## 19.3 What the single address space changes

Three things are easier than on Unix, and one is harder.

- **Easier: a PC names one image, system-wide.** VA = PA, so at any moment an address belongs to at most one Area. If the kernel records which image an Area holds (§19.4), any PC from anywhere (a fault, a profiler sample, a pointer in a shared ring) resolves with one lookup. No per-process maps are needed.
- **Easier: the debugger reads memory directly.** A pointer means the same thing in the debugger and the debuggee. With an Area shared to it read-only (§19.6), the debugger follows the debuggee's pointers itself instead of asking the kernel for each word, as ptrace makes a debugger do.
- **Easier: watchpoints see shared state.** A hardware watchpoint on a word in a shared Area catches the write from whichever process makes it. That is exactly the torn-shared-state case [§12.5](12-let-it-crash.md#125-shared-memory-without-state-spill) worries about.
- **Harder: code can't be copied privately.** Unix plants a breakpoint in a private copy-on-write copy of a shared page. Here, a copy would have a different physical address, so it would be at a different address. That is harmless for a program's own code, since `RunCommand` loads each process its own image Area. It matters for the library region, which every process maps (§19.7).

## 19.4 Images know who they are

The kernel records, for each code Area, an *image*:

```c
struct ImageInfo {
    void *ii_Base;                  /* where it was loaded              */
    uint64_t ii_Etext;              /* code and constants end here      */
    uint8_t ii_BuildID[16];         /* planar-ld's, from the image note */
    const void *ii_Unwind;          /* planar-c §5.1's table, or null   */
    uint64_t ii_UnwindEntries;
    char ii_Name[16];
};

int64_t QueryImage(const void *pc, struct ImageInfo *info);
```

- **Programs.** `dos.library`'s loader (`lib/dos/run.c`) already reads the program headers. It also reads `PT_PLANAR_UNWIND` and the build-id note, and passes them in three new `struct ProcessImage` fields (`pi_BuildID`, `pi_Unwind`, `pi_UnwindEntries`). The kernel checks that the table lies inside the image's code, below `pi_Etext`, and records it with the Area.
- **Libraries.** `struct LibraryHeader` gains the same three fields. `kernel/library.c` records an image per library as it relocates it into the region.
- **The kernel itself** has an image too, built from its own link, so its panics unwind the same way (§19.9).
- **Cost:** about 64 bytes per loaded image in the kernel. `QueryImage` is the lookup `QueryArea` already does, plus a field.

## 19.5 Crash records

`process_fault` keeps its line and adds the image and a backtrace:

```
planar: shell: write fault 0x80412340 (pc 0x80201a3c)
planar: shell: image shell 9f2c41d0e8b7a6345c1d2e3f40516273 at 0x80200000
planar: shell: image exec.library 3b8e…  at 0x80010000
planar: shell: #0 0x80201a3c shell+0x1a3c
planar: shell: #1 0x80201f10 shell+0x1f10
planar: shell: #2 0x80010488 exec.library+0x488
planar: shell: #3 0x80200124 shell+0x124
```

- **The backtrace** comes from the faulting task's registers and planar-c's unwind table (`dbg_unwind_step`, copied into the kernel). Every stack read is checked to lie inside that task's stack Area, and at most 32 frames are printed. An `UNKNOWN` entry (planar-c §5.1) ends the walk unless the frame-pointer chain is valid.
- **The image lines** list each image the backtrace touched, once, with its full build-id.
- **On the host,** `planar-symbolize` reads these lines from the serial log (as `tools/boot-check.py` already reads it), finds each debug file by build-id in the build tree, and prints functions, inlined frames and source lines.
- **On the board,** the process's watcher (its supervisor, or the Shell for a command) can do the same with debug files from the `DEBUG:` assign (VQ49). `crash.library`, a thin wrapper over planar-c's `dbg`, is the proposed home for that code.

This step needs nothing else from this document and gives most of its value, so it comes first (§19.11).

## 19.6 The debug port

A debugger debugs a process through a port the kernel creates for it:

```c
int64_t DebugProcess(Handle process, uint32_t flags);   /* port handle, or error */
```

- **Who may.** The caller must hold the process's handle with a new `RIGHT_DEBUG`. Whoever created the process gets that right with the handle. `init` can grant it to the Shell's `Debug` command for any process (VQ47).
- **Events** arrive on the port as messages, with the stopped task's registers inline:
  - `DBG_FAULT`: the fault kind, the address, and the task's registers. The task is stopped, not killed.
  - `DBG_BREAK` and `DBG_STEP`.
  - `DBG_TASK`: a task was created or ended.
  - `DBG_IMAGE`: the process loaded an image.
  - `DBG_EXIT`.
- **Requests** go to the same port with `DoMsg`:
  - `STOP` and `RESUME`, for one task or all of them;
  - `GETREGS` and `SETREGS`;
  - `BREAK` and `UNBREAK`;
  - `STEP`;
  - `WATCH` and `UNWATCH`;
  - `SHARE`: give the debugger read (or read and write) access to one of the debuggee's Areas, so it can read memory directly (§19.3).
- **Faults while debugged.** `process_fault` checks for a debug port first.
  - If there is one, the faulting task and its siblings stop, and the process's Areas, handles and ports stay as they were. The supervisor sees the process as *held*, not dead. Its clients see their calls wait, and the [§12.7](12-let-it-crash.md#127-the-supervision-tree) deadlines still apply to them.
  - When the debugger detaches or sends `RESUME` with `KILL`, the normal fault path runs: reclaim, `ERR_PORT_DIED`, restart.
  - With no debug port, nothing changes from today.
- **Policy instead of an attached debugger.** A process can be marked "hold on fault" (`DebugProcess` with `DBGF_HOLD` and no reader yet). A debugger can then attach after the crash, which is how post-mortem works on the board without a core file (VQ46).

## 19.7 Breakpoints, stepping and watchpoints

- **Breakpoints in a program's image.** The kernel writes `BRK #0xF000` (AArch64) or `EBREAK` (RV64) through its direct map, then does the cache maintenance for that page. Image code stays read-only and executable to the process: W^X holds, because only the kernel writes.
- **Breakpoints in the library region.** That code is shared by every process (§19.3). The kernel keeps a table of planted addresses.
  - A trap from a task that isn't being debugged at that address runs the displaced instruction from a per-task out-of-line slot (one RX page per CPU, written by the kernel) and continues, as Linux uprobes do.
  - PC-relative instructions (`adrp`, `adr`, `bl`, branches, `auipc`, `jal`) are emulated instead of moved.
  - The cost falls only on tasks that run past a planted address, and only while it is planted. VQ48 asks whether hardware breakpoints alone would be enough.
- **Stepping.**
  - AArch64 steps in hardware: `MDSCR_EL1.SS` and the saved `SPSR.SS`, per task, switched with the task.
  - RV64 has no single-step outside debug mode, so the debugger decodes the instruction at the PC and plants temporary breakpoints at its successors, using planar-c's instruction knowledge (`src/rv64.h`).
- **Watchpoints.** These use the hardware: AArch64 debug watchpoint registers, and RISC-V `Sdtrig` triggers where the SBI exposes them. They are per task and switched with it, or, with `WATCHF_ALL`, set for every task (§19.3). The number available depends on the core and is reported to the debugger.
- **Assembly.** planar-c can't describe `.S` code, so the unwinder sees `UNKNOWN`. `.S` functions either keep the frame-record convention (x29/x30 at the frame pointer on AArch64, ra and s0 below s0 on RV64) or are leaves that touch neither sp nor the return-address register. `arch/*/asm.h` gains `FUNC_FRAME` and `FUNC_LEAF` macros that do this and mark the symbol's size (VQ52).

## 19.8 The debugger and its transports

The debugger core is a library: planar-c's `dbg` (tables, unwinding, symbols, variables), plus a session layer that speaks the debug-port messages. It has three transports:

| Transport | Where the debugger runs | How it reaches the debuggee |
|---|---|---|
| **Local** | On the board, as a Shell command and later a Workbench window | The debug port directly |
| **Serial** | On the host | `debugd`, a small server on the board, relays debug-port messages over the console UART, framed and checksummed like `tools/xmodem.py`. Memory reads go through `debugd` in large blocks. |
| **QEMU** | On the host, for the kernel | QEMU's gdbstub. Only its register, memory and breakpoint packets are used (§19.9). |

The messages are the same structures on every transport, as planar-c's manifest is the same bytes in memory, in a file or in a message.

**What runs where.**

- **On the host:** everything from day one, including expression evaluation (planar-c §7), since planar-c runs there.
- **On the board:**
  - backtraces, symbols, breakpoints, stepping, and printing variables and types, all from the debug file;
  - expression evaluation once planar-c's library core runs on Planar. Its arenas sit behind one small file (`src/arena_posix.c`), so the port is an `arena_planar.c` over `AllocArea` plus a `dos.library` file layer (VQ50).

**Debug files on the board** live under the `DEBUG:` assign, named by build-id (`DEBUG:9f2c41d0….dbg`), so they are never confused when images are rebuilt. The image itself carries only its build-id and unwind table (VQ49).

## 19.9 Kernel debugging

- **Under QEMU,** the host debugger uses QEMU's gdbstub as its transport. It reads the kernel's image from the build tree and its debug file by build-id, and uses the same `dbg` core for everything else.
- **On a board, a kernel panic** prints the same crash record as §19.5, unwound through the kernel's own image. The panic path walks only the kernel stack, with the same 32-frame bound.
- **Frames the unwinder can't describe.** The kernel is built by clang until planar-c has an assembler (VQ53), so its functions are `UNKNOWN`. Its panics fall back to the frame-pointer chain, and to symbols from the ELF symbol table.
- **Live debugging of a kernel on a board** needs JTAG (OpenOCD) and is out of scope until a board needs it.

## 19.10 Not now: record and replay

Determinism (P5) and message-based IPC make record and replay look nearly free: record what each process receives, then replay it. The zero-copy paths break that:

- [§4.3](04-ipc.md#43-fast-path-no-trap-while-the-receiver-is-awake)'s rings deliver data without a trap while the receiver is awake;
- lent Areas (`IOF_LEND`) are read in place;
- GPU queues write memory directly.

Shared memory is what makes rr hard on Linux too. Replaying one process would mean logging every ring and lent Area it reads, which costs exactly the copies Planar exists to avoid. Revisit once rings have a trace mode (VQ51).

## 19.11 Steps and tests

Each step pairs with a planar-c milestone (D1–D4 in its §9, placed as M6 and M8–M10 in its ilr-design §11). The roadmap places the steps ([§8](08-roadmap.md#debugging)).

1. **Crash records** (with D1, planar-c M6):
   - first, in the clang build: `-fno-omit-frame-pointer` everywhere, plus `-mno-omit-leaf-frame-pointer` on AArch64. clang drops the frame pointer on RV64 by default, which breaks the chain this step falls back to. Also ld.lld's `--build-id=md5`, so every image has the 16-byte id planar-ld writes;
   - `struct ImageInfo`, `QueryImage`, and the image fields in `ProcessImage` and `LibraryHeader`;
   - the unwinder in the kernel and §19.5's log;
   - `planar-symbolize`.

   *Tested by* a conformance program that faults three calls deep, through exec.library. Its serial transcript must show the right images and frames, and `planar-symbolize` must turn them into the expected source lines (the conformance-by-transcript method).
2. **The debug port** (with D2, planar-c M8; after M7 builds all of user space):
   - `DebugProcess`, the events and requests, and hold-on-fault;
   - breakpoints in program images;
   - AArch64 hardware step, and RV64 software step;
   - the on-board `Debug` command printing variables at `-O0`.

   *Tested by* a scripted `Debug` session as a transcript: break, step, print locals, continue, exit, on both ISAs. Also checked: a held process's clients time out per §12.7, and detaching kills and restarts it.
3. **Shared code and watchpoints** (with D3, planar-c M9): out-of-line breakpoints in the library region, and hardware watchpoints.

   *Tested by* a breakpoint in exec.library's `AllocVec` while another process calls it 10,000 times: it must never stop, and its results must be unchanged. A watchpoint on a shared ring word must catch the other process's write.
4. **The host debugger over serial, and expressions** (with D4, planar-c M10): `debugd`; the planar-c core running on Planar (VQ50).

## 19.12 Open questions

VQ46 to VQ53, in [questions.md](questions.md#debugging).
