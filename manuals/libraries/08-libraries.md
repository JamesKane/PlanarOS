# 8. Libraries and interfaces

*Written, V6; gfx.library and intuition.library, V7.*

A **shared library** is code that every process uses but none carries: one copy in memory, mapped into each process at the same address. Planar's are exec.library, utility.library, gfx.library ([chapter 12](12-gfx.md)) and intuition.library ([chapter 13](13-intuition.md)). A program reaches one through an **interface**, a table of its functions: `IExec->AllocVec(64, 0)` is a call through exec.library's. This is AmigaOS 4's model, with Exec's names ([docs §7.2](../../docs/07-language-toolchain.md)).

## Concepts

- **The shared library region.** At boot the kernel copies every library in the boot image into one Area, a page apart, and relocates each there once. Every process maps the whole region from its start, read and execute, and global, so the TLB keeps one entry per page whichever process runs. Nothing in it is written after boot, and it's never freed. On QEMU it's 9 pages on RV64 and 13 on AArch64, with gfx.library and intuition.library.
- **A library is code and constants.** It has no data of its own and no `.bss`: the build refuses one that does (`arch/<isa>/user/lib.ld`). What it needs per process is the program's (below).
- **Its base** is a `struct Library`: its name, version and revision, an ID string, and its interfaces. `IExec->OpenLibrary(name, version)` returns it.
- **An interface** is a `struct Interface` header, its name, version, size and library, followed by function pointers in an order fixed for that name and version. A later version only adds at the end, so a program built against version 1 runs against any later one. Every library has one called `"main"`. `IExec->GetInterface(base, "main", version)` returns it.
- **`IExec`** is exec.library's main interface. The kernel gives every process its address at the start, in the third argument register. crt0 keeps it in the program's `IExec` before `main` runs. It's `nullptr` only in the hosted build, which has no libraries.

## Opening a library

```c
#include <planar/exec.h>
#include <planar/utility.h>

static struct UtilityIFace *IUtility;

int main(void)
{
    struct Library *util = IExec->OpenLibrary("utility.library", 6);

    if (!util || !(IUtility = (struct UtilityIFace *)IExec->GetInterface(util, "main", 1)))
        return 20;                              /* too old, or not there */
    IUtility->SNPrintf(line, sizeof line, "%lu blocks", n);
    /* ... */
    IExec->DropInterface(&IUtility->Data);
    IExec->CloseLibrary(util);
    return 0;
}
```

Ask for the version whose functions you call. `OpenLibrary` refuses a library older than you asked for, and `GetInterface` an interface older than you asked for, so a program that runs has every entry it calls.

## What a library keeps for a process

Some of exec.library's calls need something of the calling process's: `AllocVec` its pool, and `CreateTask` the program's own start-up code for a task, which sets up the program's `thread_local` layout. A library can't keep either, so the program does. The first word of each task's thread block (`lib/rt/tls.c`) points at a small structure in the program's own memory, its **process data** (`lib/exec/procdata.h`). exec.library reads it through the thread pointer.

One consequence is that a program's linked-in `AllocVec` (from `libexec.a`) and the library's share one pool. A block from either can be freed by the other's `FreeVec`, which `conformance/library.c` checks.

AmigaOS 4 passes each interface function its interface as a first argument, `Self`, for the same reason. Planar's take no `Self`: the interface is the same in every process, so it couldn't say whose call it is, and the thread pointer can.

## Chosen at boot

As the kernel relocates exec.library, it points `CopyMem`, `CopyMemQuick` and `SetMem` at the vector unit's versions (`arch/<isa>/user/string.S`) where the CPU has one: RVV 1.0 on RISC-V, and NEON, always, on Arm. Otherwise they stay word loops. gfx.library's `RectFill`, `BltBitMap` and `DrawBevel` are chosen the same way (`arch/<isa>/lib/gfx.c`, built with V for the library). The libraries are built for the ISA's base, without V, so the same image runs on a hart with V or without it. SVE2 isn't used yet: the kernel doesn't save SVE's registers, so user code can't use them.

## Costs

Measured by `conformance/library.c`, under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `AllocVec` and `FreeVec`, linked in | 188 ns | 81 ns |
| The same through `IExec` | 195 ns | 83 ns |
| An empty `GetTagData` through `IUtility` | 25 ns | 11 ns |

A call through a table costs one load more than a direct call. Opening a library is a search of the directory's few names. Mapping the region costs each process page-table entries, and no memory beyond them.

## Rules

- **Check what `OpenLibrary` and `GetInterface` return.** Either is `nullptr` for a version too new.
- **Ask for the version you were built against**, not 0, so an older library is refused rather than called past its table's end.
- **Don't write to a library or its tables.** They're read-only in every process: a write faults.
- **Keep the interface in a variable of your own**, as `IUtility` above, and use it from any task. It's the same in all of them.
- **Close what you open**, though in V6 nothing counts it.
- **A library calls another only through an interface.** utility.library's `SNPrintf` uses its own copy of `RawDoFmt`, since it can't keep exec's interface anywhere.

## Differences from Exec

| Exec | Planar |
|---|---|
| `ExecBase` at absolute address 4 | `IExec` handed to each process at its start: no address is RAM on every board |
| `OpenLibrary` loads from `LIBS:` and runs the library's open function | Every library is in place from boot. `OpenLibrary` looks it up |
| A jump table of negative offsets from the base | Interfaces of function pointers, by name and version (AmigaOS 4) |
| A library's base holds its data, shared by all callers | A library holds none; what it needs per process is the program's |
| `lib_OpenCnt` counts opens, and an unused library may be expunged | Nothing is counted or unloaded in V6 |
| Interface functions take `Self` (AmigaOS 4) | They don't: the thread pointer finds the caller's data |

## Functions used

[CloseLibrary](../autodocs/exec.md#execcloselibrary),
[DropInterface](../autodocs/exec.md#execdropinterface),
[GetInterface](../autodocs/exec.md#execgetinterface),
[OpenLibrary](../autodocs/exec.md#execopenlibrary),
[QueryProcess](../autodocs/exec.md#execqueryprocess).
