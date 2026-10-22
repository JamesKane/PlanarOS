# 1. Introduction to Planar programming

*Checked against V5.*

Planar is a single-user system in the Amiga tradition: tasks wait on signals, talk by passing pointers to messages, and the system stays small. Unlike AmigaOS, programs run in **protection domains**: a program can only touch the memory it has been given. This chapter is the vocabulary for the rest of the volume.

## One address space

Every process sees memory at the same addresses. If two processes can both see a buffer, it is at the same address in both, so a pointer can be passed from one to the other and used as it is. That is what makes Exec-style IPC work with protection.

What differs between processes is *which* memory they can see, and with what rights. Memory comes in **Areas** (chapter 3): page-rounded blocks that the kernel maps into one process or several.

Planar's layout of the address space is **identity**: an address is the physical address (VQ3, decided 2026-10-20, [docs §3.1](../../docs/03-memory.md)).Every process that maps an Area sees it at the same address, and an Area is one physically contiguous run of memory.

**Don't assume where an Area will be.** Two Areas allocated one after the other needn't be next to each other, and a large Area can fail to allocate when memory is fragmented even though enough is free in total.

## Processes and tasks

| Object | What it is | Lifetime |
|---|---|---|
| **Process** | A protection domain plus the Areas mapped into it, the handles it owns, and one or more tasks. | Ends with `ExitProcess`, when its last task ends, when it faults, or when it's killed (`KillProcess`, or its handle closed). |
| **Task** | A thread of execution in a process: registers, a 16 KiB stack, a priority, 64 signal bits. | Ends with `ExitTask`, or with its process. |

A program is a process. Its first task starts at `main()`, and **returning from `main()` calls `ExitProcess()`** with the return value (`arch/<isa>/user/crt0.S`).

A process starts another with `CreateProcess`, for a program built into the kernel image, or `CreateProcessImage`, for one its caller has loaded into an Area. `dos.library`'s `dos_run` loads a program from storage that way ([DOS ch. 1](../dos/01-files-and-programs.md)), and the Shell runs commands with it. The new process's handle belongs to the caller's process:
- The calling task gets the signal it named when the new process is dead, and `QueryProcess` then says how it ended: its exit code, or the fault and where.
- **A process never outlives its handle.** `CloseHandle` kills it if it's still running, and so does its owner's death.
- `PROCF_HELD` creates it without starting it, so the caller can give it what it needs first (hwdb grants a driver its leases this way), then `StartProcess`.

When a process ends, for any reason, everything it held goes: its tasks, its handles, its own Areas, its ports and its connections. The other end of each connection is told (chapter 4).

## Handles

The kernel names its objects (tasks, Areas, ports, connections, processes and device leases) with **handles**: 32-bit values of type `Handle`, defined in `planar/exec.h`.
- A handle names the same object **system-wide**, but what another process can do with it is limited. A task's handle given to another process lets that process share an Area with it, and only while the two are connected (chapter 4). `Signal` reaches only tasks in the caller's own process; across processes, a connection's `SignalConn` reaches its peer.
- Most calls also check **ownership**: only the process that allocated an Area can free or share it, and only a port's owner can accept on it. The autodoc for each call says which.
- Handles aren't passed between processes. A handle's value can be sent in a message to *name* an object, as hwdb names a driver's leases, but only rights the kernel has given the receiver let it use it.
- A handle is never 0. A handle that has been freed is not reused for a long time: each slot carries a generation number, so a stale handle fails with `ERR_BADHANDLE` rather than reaching a new object.

## Kernel calls and errors

Kernel calls are ordinary C functions. Each is a short stub that traps into the kernel (`syscalls.S`). The trap ABI is in `planar/syscalls.h` and matters only to someone writing stubs for another language.

Errors are small negative numbers, the `ERR_*` constants in `planar/syscalls.h`:

| Error | Value | Usually means |
|---|---|---|
| `ERR_NOSYS` | -1 | No such kernel call |
| `ERR_BADADDR` | -2 | A pointer argument isn't mapped, or isn't writable |
| `ERR_BADHANDLE` | -3 | Not a live handle, the wrong kind, or not yours |
| `ERR_NOMEM` | -4 | Out of memory, or the system's handle table is full |
| `ERR_NOTFOUND` | -5 | No such name or object; or nothing waiting |
| `ERR_BADARG` | -6 | An argument out of range |
| `ERR_EXISTS` | -7 | The name or mapping already exists |
| `ERR_RIGHTS` | -8 | Rights an Area's flags or W^X don't allow; or sharing with a process the caller isn't connected to |
| `ERR_FULL` | -9 | A ring is full, or a port's queue of waiting connections |
| `ERR_PORT_DIED` | -10 | The other end of a connection has gone |
| `ERR_LIMIT` | -11 | A per-process limit: handles, tasks or connections (below) |
| `ERR_BUSY` | -12 | In use: a lease that is granted, or stopping |
| `ERR_TIMEOUT` | -13 | A deadline passed first (`WaitAddr`) |

Most calls return `int64_t`: 0 or a handle on success, a negative error on failure. A call that makes a handle returns it as a positive `int64_t`, so keep the result in an `int64_t`, test it with `< 0`, and cast it to `Handle` once it's known to be one:

```c
int64_t a = AllocArea(size, 0, &p);
if (a < 0)
    return a;                   /* ERR_NOMEM, ERR_LIMIT, ... */
FreeArea((Handle)a);
```

**One call still returns `Handle`**: `FindTask`. `Handle` is unsigned 32-bit, so its error comes back as `(Handle)ERR_NOTFOUND`, and `(int64_t)h > 0` does **not** catch it. `FindTask(nullptr)` can't fail.

## What every program links

There's no C library yet. Every program links the runtime instead (`lib/rt`, `lib/exec`, `third_party/compiler-rt`), which has what C compiled freestanding needs:
- **Start and end.** crt0 relocates the program, makes its first task's `thread_local` variables, calls `main`, and passes its return value to `ExitProcess`.
- **The memory functions** `memcpy`, `memmove`, `memset` and `memcmp`. The compiler calls them for struct copies and initialisers, and a program may too. Exec's names for them are `CopyMem`, `CopyMemQuick` and `SetMem` (`planar/exec.h`). On both ISAs the first three use the vector unit, RVV or NEON, 32 bytes or more a step; `memcmp` compares 8 bytes a step.
- **128-bit arithmetic.** `__int128` and `_BitInt(128)` division and modulo call compiler-rt's builtins. A program links only the ones it uses.
- **`thread_local`**, a copy for each task (chapter 2). A variable can be aligned to 64 bytes at most: the build refuses a program with more.

A program also links, if it calls them, exec's pools and `AllocVec` (chapter 3) and `RawDoFmt`, the formatter ([autodoc](../autodocs/exec.md#execrawdofmt)), and utility.library: tag lists, hooks, `SNPrintf`, `Span` and `Str` (chapter 5). They're in archives, `libexec.a` and `libutility.a`, so a program that doesn't call them doesn't carry them.

Measured by `conformance/memfns.c`, 64 KiB at a time, under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `memcpy` | 5.0 GB/s | 8.0 GB/s |
| `memset` | 6.5 GB/s | 9.7 GB/s |
| A byte loop, for comparison | 0.66 GB/s | 0.41 GB/s |

QEMU's figures say only how many instructions each takes. The boards' are measured when each is next on the bench.

## Limits

So that no one process can use up what every process shares, each has limits. Going past one gives `ERR_LIMIT`.

| Limit | Per process | Counts |
|---|---|---|
| Handles | 256 | Every handle the process holds: its tasks, Areas, ports, connection ends, and the processes and leases it made |
| Tasks | 64 | Including the one running `main()` |
| Connections | 32 | Connection ends, client and server, open or still waiting to be accepted |

A port also queues at most 32 connections waiting to be accepted. Past that, `OpenPort` and `ConnectPort` fail with `ERR_FULL`: the server isn't keeping up, so try later.

The limits are the kernel's (`kernel/process.h`, `kernel/port.c`), not in a public header. There is no limit yet on the memory a process allocates, other than what is free ([docs §10](../../docs/10-code-review.md), M11). `conformance/limits.c` runs into each of the three.

## Logging

A program writes text for the user with `dos.library`'s console calls (`con_open`, `con_puts`), which go to the screen where there is one ([Devices ch. 3](../devices/03-console.md)). Underneath, and for anything that is only for the log, a program writes a line to the serial console with `KLog("text")`. The kernel prefixes it with the program's name and writes any byte that isn't printable ASCII as `?`. Each process may write 32 lines at once and 20 a second after that; lines past the limit are dropped and counted, and the next line written says how many. The conformance programs format numbers into a buffer first (`conformance/log.h`).

## Names in this volume

Public names follow Exec: `CamelCase` functions, structures with a two-letter field prefix (`mn_Length`, `ai_Size`), `AREAF_` for flags and `AREAR_` for rights. Fixed-width `<stdint.h>` types replace Exec's `ULONG` and `APTR` (VQ18).

## Functions used

[CloseHandle](../autodocs/exec.md#execclosehandle),
[CopyMem](../autodocs/exec.md#execcopymem),
[CreateProcess](../autodocs/exec.md#execcreateprocess),
[CreateProcessImage](../autodocs/exec.md#execcreateprocessimage),
[ExitProcess](../autodocs/exec.md#execexitprocess),
[KillProcess](../autodocs/exec.md#execkillprocess),
[KLog](../autodocs/exec.md#execklog),
[QueryProcess](../autodocs/exec.md#execqueryprocess),
[StartProcess](../autodocs/exec.md#execstartprocess).
