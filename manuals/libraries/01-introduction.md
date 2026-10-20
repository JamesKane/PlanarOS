# 1. Introduction to Planar programming

*Checked against V2.*

Planar is a single-user system in the Amiga tradition: tasks wait on signals, talk by passing pointers to messages, and the system stays small. Unlike AmigaOS, programs run in **protection domains**: a program can only touch the memory it has been given. This chapter is the vocabulary for the rest of the volume.

## One address space

Every process sees memory at the same addresses. If two processes can both see a buffer, it is at the same address in both, so a pointer can be passed from one to the other and used as it is. That is what makes Exec-style IPC work with protection.

What differs between processes is *which* memory they can see, and with what rights. Memory comes in **Areas** (chapter 3): page-rounded blocks that the kernel maps into one process or several.

Two layouts of the address space are built, and which one Planar keeps is still being measured (VQ3, [docs §3.1](../../docs/03-memory.md)):
- **identity**: an address is the physical address;
- **global-VA**: addresses come from a system-wide allocator, as in omega.

**A program must not assume either.** In particular, don't assume an Area's address is its physical address, or that two Areas allocated one after the other are next to each other.

## Processes and tasks

| Object | What it is | Lifetime |
|---|---|---|
| **Process** | A protection domain plus the Areas mapped into it, the handles it owns, and one or more tasks. | Ends with `ExitProcess`, when its last task ends, or when it faults. |
| **Task** | A thread of execution in a process: registers, a 16 KiB stack, a priority, 64 signal bits. | Ends with `ExitTask`, or with its process. |

A program is a process. Its first task starts at `main()`, and **returning from `main()` calls `ExitProcess()`** with the return value (`arch/<isa>/user/crt0.S`).

There is no call yet to start a new program. Programs are linked into the kernel image and started by it at boot. A loader and a shell come with storage (V4).

When a process ends, for any reason, everything it held goes: its tasks, its handles, its own Areas, its ports and its connections. The other end of each connection is told (chapter 4).

## Handles

The kernel names its objects (tasks, Areas, ports, connections) with **handles**: 32-bit values of type `Handle`, defined in `planar/exec.h`.
- A handle is valid **system-wide**. A task's handle, for instance, can be given to another process, which can then `Signal` it or share an Area with it.
- Some calls also check **ownership**: only the process that allocated an Area can free or share it, and only a port's owner can accept on it. The autodoc for each call says which.
- A handle is never 0. A handle that has been freed is not reused for a long time: each slot carries a generation number, so a stale handle fails with `ERR_BADHANDLE` rather than reaching a new object.

## Kernel calls and errors

Kernel calls are ordinary C functions. Each is a short stub that traps into the kernel (`syscalls.S`). The trap ABI is in `planar/syscalls.h` and matters only to someone writing stubs for another language.

Errors are small negative numbers, the `ERR_*` constants in `planar/syscalls.h`:

| Error | Value | Usually means |
|---|---|---|
| `ERR_NOSYS` | -1 | No such kernel call |
| `ERR_BADADDR` | -2 | A pointer argument isn't mapped, or isn't writable |
| `ERR_BADHANDLE` | -3 | Not a live handle, the wrong kind, or not yours |
| `ERR_NOMEM` | -4 | Out of memory or out of handles |
| `ERR_NOTFOUND` | -5 | No such name or object; or nothing waiting |
| `ERR_BADARG` | -6 | An argument out of range |
| `ERR_EXISTS` | -7 | The name or mapping already exists |
| `ERR_RIGHTS` | -8 | Rights an Area's flags don't allow |
| `ERR_FULL` | -9 | A ring is full |
| `ERR_PORT_DIED` | -10 | The other end of a connection has gone |

Most calls return `int64_t`: 0 or a handle on success, a negative error on failure.

**Watch out for calls declared to return `Handle`** (`AllocArea`, `CreateTask`, `FindTask`). `Handle` is unsigned 32-bit, so an error comes back as the error converted to `Handle`. Test it like this:

```c
Handle a = AllocArea(size, 0, &p);
if (a == (Handle)ERR_NOMEM || a == (Handle)ERR_BADARG)
    ...
```

A valid handle's low 16 bits are below 4096, and an error's are above 0xFFF0, so the two never collide. Testing `(int64_t)a > 0` does **not** catch errors. See BUGS under each of those calls.

## Logging

Until there is a console device, a program writes a line to the serial console with `KLog("text")`. The kernel prefixes it with the program's name. The conformance programs format numbers into a buffer first (`conformance/log.h`).

## Names in this volume

Public names follow Exec: `CamelCase` functions, structures with a two-letter field prefix (`mn_Length`, `ai_Size`), `AREAF_` for flags and `AREAR_` for rights. Fixed-width `<stdint.h>` types replace Exec's `ULONG` and `APTR` (VQ18).

## Functions used

[KLog](../autodocs/exec.md#execklog), [ExitProcess](../autodocs/exec.md#execexitprocess).
