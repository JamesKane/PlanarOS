# exec: kernel calls

*Checked against V5; pools, `AllocVec`, `RawDoFmt`, the library calls and the deadlines (`OpenPort`, `SignalWaitConn`, `WaitUntil`), V6.*

The kernel calls, in alphabetical order, and exec's library functions over them: those every program links, and the pools, `AllocVec` and `RawDoFmt` (`libexec.a`), which a program links if it calls them. Each kernel call is one stub in `arch/<isa>/user/syscalls.S` and traps on every call; a library function says so. The headers are `planar/exec.h` (tasks, signals, Areas, processes and time) and `planar/ipc.h` (ports and connections). The device-lease calls are in [lease.md](lease.md). Errors are listed in [Libraries ch. 1](../libraries/01-introduction.md#kernel-calls-and-errors), and the per-process limits behind `ERR_LIMIT` [there too](../libraries/01-introduction.md#limits).

Most programs use connections through the [msg library](msg.md) rather than the connection calls here.

exec.library, in the shared library region, has the calls here that aren't about ports and connections, by the same names, in its main interface: `IExec->AllocVec(...)` ([Libraries ch. 8](../libraries/08-libraries.md)). `OpenLibrary`, `GetInterface`, `DropInterface` and `CloseLibrary` are reached only that way.

---

## exec/AcceptConn

**NAME**
AcceptConn -- accept the oldest connection waiting on a port

**SYNOPSIS**
```c
#include <planar/ipc.h>
int64_t AcceptConn(Handle port, int64_t sigbit, struct ConnArena **arena);
```

**FUNCTION**
Takes the oldest connection queued on `port` and maps its arena into the calling process, read-write, at the address the client sees it at. From then on the calling task is the connection's server end. Each time the client calls `SignalConn` or `SignalWaitConn` on this connection, signal bit `sigbit` is set in the calling task.

A kernel call.

**INPUTS**
- `port` -- a port handle owned by the calling process.
- `sigbit` -- the signal bit, 16–63, to be woken with for this connection.
- `arena` -- where to store the arena's address.
- `deadline` -- how long to wait for the name, on `CurrentTime`'s clock: 0 not at all, `DEADLINE_NONE` (`planar/syscalls.h`) for ever.

**RESULT**
The connection's handle, or:
- `ERR_BADARG` -- `sigbit` is out of range.
- `ERR_BADADDR` -- `arena` isn't writable.
- `ERR_BADHANDLE` -- `port` isn't a port this process owns.
- `ERR_NOTFOUND` -- no connection is waiting.
- `ERR_LIMIT` -- the calling process already holds 32 connection ends, or 256 handles.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.

**NOTES**
- The kernel doesn't check that `sigbit` was allocated with `AllocSignal`.
- A connection whose client went before it was accepted can still be accepted. Its arena already says `CONNF_CLIENT_GONE`.
- The connection belongs to the calling *task*. It closes when that task ends.
- The arena layout is checked by `AcceptPort`, not by this call.
- Each connection accepted counts against the process's 32 connection ends, client ends included. A server serves at most 32 clients at once, fewer if it is also a client.
- If `arena` stops being writable during the call, the result is `ERR_BADADDR` but the connection has been accepted. It's closed when the task ends.

**BUGS**
In that last case the handle is lost to the caller, as with `OpenPort`.

**SEE ALSO**
[AcceptPort](msg.md#execacceptport), [CreatePort](#execcreateport), [OpenPort](#execopenport), [CloseConn](#execcloseconn)

---

## exec/AllocArea

**NAME**
AllocArea -- allocate an Area of memory

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t AllocArea(uint64_t size, uint32_t flags, void **address);
```

**FUNCTION**
Allocates `size` bytes, rounded up to whole pages, and maps them read-write into the calling process. The memory is zeroed. The calling process owns the Area.

A kernel call.

**INPUTS**
- `size` -- bytes wanted; more than 0.
- `flags` -- any of `AREAF_CLEAR`, `AREAF_CONTIG`, `AREAF_EXEC`, `AREAF_RWX`, `AREAF_LARGE`, `AREAF_DMA32`, `AREAF_NC`. See [Libraries ch. 3](../libraries/03-areas.md#flags). `AREAF_DMA32` places the Area below 4 GiB, for memory a 32-bit DMA engine must reach. `AREAF_NC` maps it uncached, in every process that maps it, for memory a device reads without snooping the CPU's caches. Writes to it may be gathered, so put a barrier between them and a doorbell.
- `address` -- where to store the Area's address.

**RESULT**
The Area's handle (positive), or:
- `ERR_BADARG` -- `size` is 0, or `flags` has an unknown bit.
- `ERR_NOMEM` -- out of memory, no run of free pages is long enough (below 4 GiB with `AREAF_DMA32`), or the system's handle table is full. A `size` of more pages than are free fails at once, before anything is allocated.
- `ERR_NOSYS` -- `AREAF_NC`, on a CPU that can't map memory uncached (RISC-V without Svpbmt).
- `ERR_LIMIT` -- the calling process already holds 256 handles.
- `ERR_BADADDR` -- `address` isn't writable. The Area has been freed again.
- `ERR_BADHANDLE` -- another task of the process freed the new handle before this call returned.

**NOTES**
- The Area starts with `AREAR_READ | AREAR_WRITE` rights, even with `AREAF_EXEC`. Use `ProtectArea` to make it executable.
- `AREAF_CLEAR` changes nothing: every Area is zeroed. `AREAF_CONTIG` changes nothing either: an Area's address is its physical address, so every Area is one physical run.
- A large Area can fail with `ERR_NOMEM` while enough memory is free in total, if it's fragmented. Under heavy load, about 0.15% of allocations do (VQ3).
- Keep the result in an `int64_t` and test it with `< 0`; cast to `Handle` only once it's known to be one.

**BUGS**
None known.

**SEE ALSO**
[FreeArea](#execfreearea), [ProtectArea](#execprotectarea), [QueryArea](#execqueryarea), [ShareArea](#execsharearea)

---

## exec/AllocPooled

**NAME**
AllocPooled -- allocate a block from a pool

**SYNOPSIS**
```c
#include <planar/exec.h>
void *AllocPooled(struct Pool *pool, uint64_t memSize);
```

**FUNCTION**
Returns a block of at least `memSize` bytes from `pool`, 16-byte aligned. A block up to the pool's threshold comes from its size class's free list, or from the current puddle; when the puddle is used up, the pool allocates another. A block above the threshold is an Area of its own, recorded on the pool so that `DeletePool` frees it.

A library function (`lib/exec/pool.c`, in `libexec.a`). It traps only to allocate an Area, for a new puddle or a block above the threshold.

**INPUTS**
- `pool` -- from `CreatePool` or `CreatePoolMem`.
- `memSize` -- the block's size in bytes.

**RESULT**
The block, or `nullptr` if:
- `pool` is `nullptr`, or `memSize` is 0;
- the Area couldn't be allocated (out of memory, or the process holds 256 handles);
- the pool is in a fixed buffer, and it's full, or `memSize` is more than the buffer holds.

**NOTES**
- The block isn't cleared unless the pool was created with `AREAF_CLEAR`. A block from a fresh puddle is zero anyway; one taken again from a free list holds what it held.
- Sizes are rounded up to their class: 16-byte steps to 256 bytes, then four classes to each doubling (320, 384, 448, 512, 640, …). A block wastes at most a fifth of itself, and no header.
- When a block doesn't fit in what's left of the current puddle, the rest of that puddle goes unused.
- A pool without `POOLF_LOCKED` is for one task at a time.

**BUGS**
None known.

**SEE ALSO**
[CreatePool](#execcreatepool), [FreePooled](#execfreepooled), [AllocVec](#execallocvec), [Libraries ch. 3](../libraries/03-areas.md#pools)

---

## exec/AllocSignal

**NAME**
AllocSignal -- allocate a signal bit

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t AllocSignal(int64_t bit);
```

**FUNCTION**
Allocates a signal bit for the calling task and clears it. With `bit` = -1, takes the lowest free bit from 16 to 63.

A kernel call.

**INPUTS**
- `bit` -- the bit wanted, 16–63, or -1 for any.

**RESULT**
The bit's number, or -1 if it's taken, out of range, or none is free.

**NOTES**
- -1 here is not `ERR_NOSYS`: it's Exec's "no signal".
- Bits are per task. Allocating in one task doesn't reserve the bit in others.

**BUGS**
None known.

**SEE ALSO**
[FreeSignal](#execfreesignal), [Wait](#execwait), [Libraries ch. 2](../libraries/02-tasks-and-signals.md)

---

## exec/AllocVec

**NAME**
AllocVec -- allocate memory that remembers its size

**SYNOPSIS**
```c
#include <planar/exec.h>
void *AllocVec(uint64_t byteSize, uint32_t requirements);
```

**FUNCTION**
Returns a block of `byteSize` bytes, 16-byte aligned, to be freed by `FreeVec` without its size. A 16-byte header before the block records the size. Blocks come from a pool the process makes at its first `AllocVec`, with 64 KiB puddles and a 16 KiB threshold. The pool is locked, so the process's tasks may all use it at once.

With `AREAF_DMA32` or `AREAF_NC`, the block is instead an Area of its own, with those flags. It starts 16 bytes in.

A library function (`lib/exec/pool.c`, in `libexec.a`). It traps when the pool needs an Area, or when a task has to wait for another to finish with the pool.

**INPUTS**
- `byteSize` -- the block's size in bytes.
- `requirements` -- 0, or any of:
  - `AREAF_CLEAR` -- zero the block.
  - `AREAF_DMA32` -- below 4 GiB, as an Area of its own.
  - `AREAF_NC` -- not cached by the CPU, as an Area of its own.
  - `AREAF_CONTIG` -- accepted and ignored, as for `AllocArea`: all memory is contiguous.

**RESULT**
The block, or `nullptr` if:
- `byteSize` is 0, or so large that the header would overflow it;
- `requirements` has any other flag, such as `AREAF_EXEC` or `AREAF_LARGE`;
- the memory or an Area's handle couldn't be had.

**EXAMPLE**
From `conformance/pool.c`:
```c
uint8_t *w = AllocVec(n, AREAF_CLEAR);
/* ... */
FreeVec(w);
```

**NOTES**
- **Differs from Exec:** Exec's `AllocVec` took `MEMF_*` flags, and every task could see what it returned. Here the block is the process's: its tasks share it, and no other process sees it. To give memory to another process, allocate an Area and share that ([Libraries ch. 3](../libraries/03-areas.md)).
- Without `AREAF_CLEAR`, a block taken again from the pool holds what it held.
- A block of more than 16,368 bytes (16 KiB less the header) is above the pool's threshold, and so an Area of its own, page-rounded with its header: one of exactly 64 KiB takes 17 pages.
- `AREAF_DMA32` places the block, not the device's view of it. A driver's DMA memory is `AllocDmaArea`'s ([Devices ch. 1](../devices/01-drivers.md)).

**BUGS**
None known.

**SEE ALSO**
[FreeVec](#execfreevec), [AllocPooled](#execallocpooled), [AllocArea](#execallocarea)

---

## exec/ArmTimer

**NAME**
ArmTimer -- set or clear the calling task's alarm

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t ArmTimer(uint64_t deadline, int64_t sigbit);
```

**FUNCTION**
Arms the calling task's one alarm: signal bit `sigbit` is set in the task once `CurrentTime` reaches `deadline`. A deadline already past sets it at once. Arming replaces any alarm armed before. A `deadline` of 0 disarms the alarm, and `sigbit` is then ignored.

A kernel call.

**INPUTS**
- `deadline` -- when to signal, in nanoseconds on `CurrentTime`'s clock; 0 to disarm.
- `sigbit` -- the signal bit, 16–63.

**RESULT**
0, or:
- `ERR_BADARG` -- `sigbit` is out of range and `deadline` isn't 0. The alarm armed before stays armed.
- `ERR_NOSYS` -- the kernel found no timer interrupt, so alarms can't fire. Disarming still succeeds.

**EXAMPLE**
A sleep, from `conformance/leaser.c`:
```c
ArmTimer(CurrentTime() + ms * 1000000, 30);
Wait(1ULL << 30);
```

**NOTES**
- The deadline is absolute. For a timeout, add to `CurrentTime()`.
- There is one alarm per task, not one per call. A library that arms it takes it from its caller.
- The alarm sets its bit and nothing clears it but the task. A bit set by an alarm that fired before it was replaced stays set. Clear it with `SetSignal(0, 1ULL << sigbit)` before waiting if that matters.
- The kernel doesn't check that `sigbit` was allocated with `AllocSignal`.
- The alarm is stopped when the task ends, so a task may end with one about to fire (`kernel/process.c`).
- How late an alarm comes depends on the machine. On QEMU's TCG a 2 ms alarm comes 450–600 µs late; `conformance/timer.c` logs it.

**BUGS**
None known.

**SEE ALSO**
[CurrentTime](#execcurrenttime), [Wait](#execwait), [AllocSignal](#execallocsignal)

---

## exec/CloseConn

**NAME**
CloseConn -- close this end of a connection

**SYNOPSIS**
```c
#include <planar/ipc.h>
int64_t CloseConn(Handle conn);
```

**FUNCTION**
Closes the calling process's end of a connection:
- sets this end's bit (`CONNF_CLIENT_GONE` or `CONNF_SERVER_GONE`) in the arena's `ca_Closed`;
- signals the other end, if it's open;
- unmaps the arena from the calling process and frees the handle.

A client end not yet accepted is taken off the port's queue. The arena is freed once neither end maps it.

The same happens, without this call, when the task holding the end ends or its process dies.

A kernel call.

**INPUTS**
- `conn` -- a connection handle owned by the calling process.

**RESULT**
0, or `ERR_BADHANDLE`.

**NOTES**
Any pointers into the arena are invalid after this call.

**BUGS**
None known.

**SEE ALSO**
[ClosePort](msg.md#execcloseport), [OpenPort](#execopenport), [AcceptConn](#execacceptconn)

---

## exec/CloseHandle

**NAME**
CloseHandle -- give up a process or lease handle

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t CloseHandle(Handle handle);
```

**FUNCTION**
Frees a handle of a kind that has no call of its own to free it:
- **A process handle** from `CreateProcess`. If the process is running or held, it is killed, as by `KillProcess`. Its record goes once it is dead.
- **A lease handle** from `CreateLease`, if the lease is idle. The owner's mapping of the registers is removed. The lease goes once none of its DMA Areas is left.

Tasks, Areas, ports and connections are let go by `ExitTask`, `FreeArea`, `DeletePort` and `CloseConn`. `CloseHandle` refuses them.

A kernel call.

**INPUTS**
- `handle` -- a process or lease handle owned by the calling process.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `handle` isn't a process or lease handle the calling process owns, or another task of the process closed it first.
- `ERR_BUSY` -- a lease that is granted or stopping. End the grantee and call `RevokeLease` first.

**NOTES**
- Closing a process handle doesn't wait for the process to die. The task that created the process is still signalled when it does.
- After the call the handle is stale and calls on it fail with `ERR_BADHANDLE`. The caller's copy of the value doesn't change, so don't keep using it.
- When a process dies, its handles go without this call: the processes it created are killed, and its leases are let go whatever their state. The DMA Areas of a lease that was stopping then stay quarantined for good, and the kernel log says so (`lease_release` in `kernel/lease.c`).
- The device root handle can't be closed. It goes with its process.

**BUGS**
The device root can't be closed: it lasts as long as its process.

**SEE ALSO**
[CreateProcess](#execcreateprocess), [KillProcess](#execkillprocess), [CreateLease](lease.md#leasecreatelease), [RevokeLease](lease.md#leaserevokelease)

---

## exec/CloseLibrary

**NAME**
CloseLibrary -- say a library is no longer used

**SYNOPSIS**
```c
#include <planar/exec.h>
IExec->CloseLibrary(struct Library *library);
```

**FUNCTION**
Says the caller has done with a library `OpenLibrary` returned. Call it once for each `OpenLibrary`, after dropping its interfaces.

Through `IExec` only: an interface function in exec.library ([Libraries ch. 8](../libraries/08-libraries.md)). It doesn't trap.

**INPUTS**
- `library` -- the library, or `nullptr`.

**RESULT**
None.

**NOTES**
- In V6 it does nothing. Nothing is unloaded, and the library region is read-only, so there's no count to keep. Code that closes what it opens keeps working when there is one.

**BUGS**
None known.

**SEE ALSO**
[OpenLibrary](#execopenlibrary), [DropInterface](#execdropinterface)

---

## exec/CopyMem

**NAME**
CopyMem -- copy memory

**SYNOPSIS**
```c
#include <planar/exec.h>
void CopyMem(const void *source, void *dest, uint64_t size);
```

**FUNCTION**
Copies `size` bytes from `source` to `dest`. The ranges may overlap: the copy is right whichever way they do.

A library function, inline in `planar/exec.h`, over the runtime's `memmove` (`lib/rt`). It doesn't trap. On both ISAs it moves 32 bytes or more a step with the vector unit (`arch/<isa>/user/string.S`).

**INPUTS**
- `source` -- the bytes to copy, readable.
- `dest` -- where to put them, writable.
- `size` -- how many. 0 copies nothing.

**NOTES**
- Exec's argument order: the source first, unlike `memcpy`'s.
- The compiler's own calls for struct copies go to the same code, so a struct assignment costs the same.
- On RISC-V, a process's first call is its first use of the vector unit, which costs a trap for the kernel to switch it on ([docs §7.4](../../docs/07-language-toolchain.md)).

**BUGS**
None known.

**SEE ALSO**
[CopyMemQuick](#execcopymemquick), [SetMem](#execsetmem)

---

## exec/CopyMemQuick

**NAME**
CopyMemQuick -- copy aligned memory that doesn't overlap

**SYNOPSIS**
```c
#include <planar/exec.h>
void CopyMemQuick(const void *source, void *dest, uint64_t size);
```

**FUNCTION**
Copies `size` bytes from `source` to `dest`, which must not overlap.

A library function, inline in `planar/exec.h`, over the runtime's `memcpy`. It doesn't trap.

**INPUTS**
- `source` -- the bytes to copy: 8-byte aligned.
- `dest` -- where to put them: 8-byte aligned.
- `size` -- a multiple of 8.

**NOTES**
- Exec's took longwords; Planar's words are 8 bytes.
- The alignment isn't checked, and nothing is faster for it: the vector unit doesn't care. It's kept for code written to Exec.

**BUGS**
None known.

**SEE ALSO**
[CopyMem](#execcopymem)

---

## exec/CreatePool

**NAME**
CreatePool, CreatePoolMem -- make a memory pool

**SYNOPSIS**
```c
#include <planar/exec.h>
struct Pool *CreatePool(uint32_t flags, uint64_t puddleSize, uint64_t threshSize);
struct Pool *CreatePoolMem(void *buf, uint64_t size);
```

**FUNCTION**
`CreatePool` makes a pool, whose blocks come from puddles: Areas of `puddleSize` bytes, rounded up to whole pages. A block above `threshSize` bytes is an Area of its own. The pool's header and free lists are at the start of its first puddle, which is allocated at once.

`CreatePoolMem` makes a pool in `size` bytes at `buf`, such as part of a connection's heap or a static array. Its header and lists are at the start of the buffer, aligned to 16, and the rest is the pool. It never allocates an Area, and never grows.

Library functions (`lib/exec/pool.c`, in `libexec.a`). `CreatePool` traps once, to allocate the first puddle.

**INPUTS**
- `flags` -- 0, or any of:
  - `AREAF_DMA32`, `AREAF_NC` -- for every Area the pool allocates ([ch. 3](../libraries/03-areas.md#flags)).
  - `AREAF_CLEAR` -- every block is zeroed.
  - `POOLF_LOCKED` -- the process's tasks may share the pool. Each call takes a lock, a futex on the pool's header.
  - `AREAF_CONTIG` -- accepted and ignored.
- `puddleSize` -- each puddle's size in bytes. 64 KiB is a good start.
- `threshSize` -- the largest block taken from a puddle. A quarter of `puddleSize` is a good start.
- `buf`, `size` -- `CreatePoolMem`'s memory, any alignment. It must stay valid for the pool's life.

**RESULT**
The pool, or `nullptr` if:
- `flags` has any other flag;
- `threshSize` is 0, or larger than `puddleSize`, or rounds up to a size class bigger than a puddle less its 16-byte header;
- the first puddle couldn't be allocated;
- `size` is too small for `CreatePoolMem`'s header and lists (`sizeof(struct Pool)` and 8 bytes for each size class up to `size`).

**NOTES**
- Each puddle and each block above the threshold is an Area, and so a handle against the process's 256.
- The header takes 64 bytes and 8 for each size class up to the threshold: 256 for a threshold of 1 KiB.
- A `CreatePoolMem` pool isn't locked. Its threshold is its size, so every block comes from the buffer.
- **Differs from Exec:** V39 took `MEMF_*` flags; these are `AREAF_*`. Exec's `MEMF_SEM_PROTECTED` is `POOLF_LOCKED`. `CreatePoolMem` is new.

**BUGS**
None known.

**SEE ALSO**
[DeletePool](#execdeletepool), [AllocPooled](#execallocpooled), [FreePooled](#execfreepooled), [Libraries ch. 3](../libraries/03-areas.md#pools)

---

## exec/CreatePort

**NAME**
CreatePort -- create a named port

**SYNOPSIS**
```c
#include <planar/ipc.h>
int64_t CreatePort(const char *name, int64_t sigbit);
```

**FUNCTION**
Creates a port: a system-wide name that clients connect to with `OpenPort`. Each time a client connects, signal bit `sigbit` is set in the calling task. The port has no message queue, only a queue of connections waiting for `AcceptConn`.

Names are the system's: only a process allowed to name ports may make one. The kernel's own processes (init and the selftests') may, a built-in program may if its parent may, and a program loaded from a disk may only if its parent gave it `PROCF_NAMES` (`CreateProcessImage`). So a command can't take `"dos"` before dos does and have every client that waits for it in `OpenPort`.

A kernel call.

**INPUTS**
- `name` -- 1 to 31 characters.
- `sigbit` -- 16–63.

**RESULT**
The port's handle, or:
- `ERR_BADARG` -- `sigbit` is out of range.
- `ERR_BADADDR` -- `name` isn't readable, is empty, or is longer than 31 characters.
- `ERR_EXISTS` -- a port with that name exists.
- `ERR_RIGHTS` -- the calling process may not name ports (V6 step 8).
- `ERR_LIMIT` -- the calling process already holds 256 handles.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.

**NOTES**
The port belongs to the calling task and goes when it ends.

**BUGS**
An over-long name gives `ERR_BADADDR`, where `ERR_BADARG` would be clearer.

**SEE ALSO**
[DeletePort](#execdeleteport), [AcceptConn](#execacceptconn), [OpenPort](#execopenport)

---

## exec/CreateProcess

**NAME**
CreateProcess -- start a program in a new process

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t CreateProcess(const char *name, uint64_t arg, int64_t sigbit, uint32_t flags);
```

**FUNCTION**
Loads the built-in program `name` into a new process and returns a handle to it, owned by the calling process. The program's `main` gets `arg` as its first argument and 0 as its second. The process runs at the priority of the caller's process, on the same CPUs.

Without `PROCF_HELD` the process starts at once. With it, the process is made but doesn't run until `StartProcess`, so the caller can grant it a lease first.

A built-in program is the system's own: it may name ports (`CreatePort`) if the caller's process may. `PROCF_NAMES` is accepted and changes nothing here.

When the process is dead, by exiting, faulting or being killed, signal bit `sigbit` is set in the calling task. `QueryProcess` then says how it ended.

A process never outlives its handle: `CloseHandle` on it kills it, and so does the death of the process that owns the handle.

A kernel call.

**INPUTS**
- `name` -- the program's name, at most 31 characters. The built-in programs are the boot bundle: the `conformance/` images, the servers (`servers/`) and the drivers (`drv/`), linked into the kernel. A program on a disk is started with `CreateProcessImage` instead.
- `arg` -- passed to the program's `main` as its first argument.
- `sigbit` -- the signal bit, 16–63, set in the calling task when the process is dead.
- `flags` -- 0, or `PROCF_HELD`, `PROCF_NAMES` or both.

**RESULT**
The process's handle (positive), or:
- `ERR_BADADDR` -- `name` isn't readable, or is longer than 31 characters.
- `ERR_BADARG` -- `sigbit` is out of range, or `flags` has an unknown bit.
- `ERR_NOTFOUND` -- no built-in program has that name.
- `ERR_NOMEM` -- out of memory, the program's image is malformed, or the system's handle table is full.
- `ERR_LIMIT` -- the calling process already holds 256 handles.

**EXAMPLE**
A driver started held, from `conformance/leaser.c`:
```c
int64_t d = CreateProcess("virtiorng", lease, 20, PROCF_HELD);

if (d < 0)
    return d;
GrantLease(lease, (Handle)d);
StartProcess((Handle)d);
Wait(1ULL << 20);                   /* it's dead */
QueryProcess((Handle)d, &info);
CloseHandle((Handle)d);
```

**NOTES**
- The death signal goes to the creating task. Any task of the process may use the handle, but if the creating task has ended, nobody is signalled.
- The signal comes even after the handle has been closed.
- The kernel doesn't check that `sigbit` was allocated with `AllocSignal`.
- The processes a process creates die with it (`handle_free_owner` in `kernel/handle.c`). A supervisor's children can't outlive it.
- **Differs from Exec**: AmigaDOS's `CreateNewProc` runs code already in memory, in the one address space. `CreateProcess` loads a program by name into a new protection domain.

**BUGS**
- An over-long name gives `ERR_BADADDR`, as with `CreatePort`, where `ERR_BADARG` would be clearer.
- A malformed built-in image gives `ERR_NOMEM`.

**SEE ALSO**
[CreateProcessImage](#execcreateprocessimage), [StartProcess](#execstartprocess), [KillProcess](#execkillprocess), [QueryProcess](#execqueryprocess), [CloseHandle](#execclosehandle), [GrantLease](lease.md#leasegrantlease)

---

## exec/CreateProcessImage

**NAME**
CreateProcessImage -- start a program its parent loaded

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t CreateProcessImage(const struct ProcessImage *image, uint64_t arg, int64_t sigbit,
                           uint32_t flags);
```

**FUNCTION**
Starts a new process running a program the caller has laid out in an Area, and returns a handle to it, as `CreateProcess` does for a built-in one. The kernel has no ELF loader (§2.2). `dos.library`'s `RunCommand` reads the file and calls this ([DOS ch. 1](../dos/01-files-and-programs.md#programs)).

`image` names the Area and its layout:

| Field | |
|---|---|
| `pi_Area` | An Area the caller allocated with `AREAF_EXEC`, owns, and alone maps |
| `pi_Etext` | Bytes of code and constants at its start: a page multiple |
| `pi_Entry` | Where the first task starts, as bytes from its start; below `pi_Etext` |
| `pi_Name` | The process's name, for its log lines (15 characters) |

The Area becomes the new process's. The caller stops mapping it and its handle goes, whatever happens next. In the child the first `pi_Etext` bytes are read and execute, the rest read and write, at the Area's own address (VQ3: the child sees it where the parent filled it).

Otherwise as `CreateProcess`: `arg` is `main`'s first argument, `sigbit` is set in the calling task when the process is dead, and `PROCF_HELD` holds it for `StartProcess`.

Unlike a built-in program, a loaded one may name ports only if given `PROCF_NAMES`, which only a process that may name ports can give (V6 step 8).

A kernel call.

**INPUTS**
- `image` -- the Area and its layout.
- `arg` -- passed to the program's `main`.
- `sigbit` -- the signal bit, 16–63.
- `flags` -- 0, or `PROCF_HELD`, `PROCF_NAMES` or both.

**RESULT**
The process's handle (positive), or:
- `ERR_BADADDR` -- `image` isn't readable.
- `ERR_RIGHTS` -- `PROCF_NAMES`, from a process that may not name ports itself.
- `ERR_BADARG` -- `sigbit` is out of range, `flags` has an unknown bit, `pi_Etext` isn't a page multiple, or `pi_Entry` isn't below it.
- `ERR_BADHANDLE` -- `pi_Area` isn't an Area the calling process owns and alone maps, wasn't made with `AREAF_EXEC`, is shorter than `pi_Etext`, is a DMA Area, or another call is using it. The Area is still the caller's.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.
- `ERR_LIMIT` -- the calling process already holds 256 handles.

**EXAMPLE**
From `lib/dos/dos.c`, once the program's segments are read into `image`:
```c
pi.pi_Area = (Handle)area;
pi.pi_Etext = etext;
pi.pi_Entry = entry;
e = CreateProcessImage(&pi, arg, sigbit, flags);
if (e < 0)
    FreeArea((Handle)area);     /* still ours if the kernel didn't take it */
```

**NOTES**
- The kernel makes the code executable and cleans it to the point of unification (`create_from` in `kernel/process.c`); the caller writes it as data.
- The taking is `area_take` in `kernel/area.c`.

**BUGS**
After `ERR_NOMEM` the Area is gone too: the kernel took it, then couldn't make the process.

**SEE ALSO**
[CreateProcess](#execcreateprocess), [AllocArea](#execallocarea), [StartProcess](#execstartprocess), [DOS ch. 1](../dos/01-files-and-programs.md#programs)

---

## exec/CreateTask

**NAME**
CreateTask -- start another task in this process

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t CreateTask(void (*entry)(uint64_t arg), uint64_t arg);
```

**FUNCTION**
Starts a task in the calling process, running `entry(arg)` on a new 16 KiB stack at the process's priority. The task may start on any CPU, before this call returns. It gets its own copy of the program's `thread_local` variables, at their first values, before `entry` runs. If `entry` returns, the task ends as if it had called `ExitTask(0)`.

A library function over one kernel call (`lib/exec/task.c`). The kernel starts the task at the runtime's trampoline (`_task_start` in `arch/<isa>/user/crt0.S`), which makes its `thread_local` block, calls `entry`, and calls `ExitTask` after.

**INPUTS**
- `entry` -- the function to run.
- `arg` -- passed to `entry`.

**RESULT**
The new task's handle (positive), or:
- `ERR_LIMIT` -- the process already has 64 tasks, or holds 256 handles.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.
- `ERR_NOTFOUND` -- the process is ending.

**NOTES**
- `entry` isn't checked. A bad address faults the new task, which ends the whole process.
- The new task may run, and end, before this call returns. The handle is then already stale, and calls on it fail with `ERR_BADHANDLE`.
- The 64 tasks include the one running `main()`.
- The `thread_local` block is at the top of the task's stack, so it comes out of the 16 KiB.

**BUGS**
None known.

**SEE ALSO**
[ExitTask](#execexittask), [FindTask](#execfindtask), [Libraries ch. 2](../libraries/02-tasks-and-signals.md)

---

## exec/CurrentTime

**NAME**
CurrentTime -- read the clock

**SYNOPSIS**
```c
#include <planar/exec.h>
uint64_t CurrentTime(void);
```

**FUNCTION**
Returns the time, in nanoseconds, on the system's monotonic clock. It never goes backwards.

A kernel call.

**RESULT**
The time in nanoseconds. Can't fail.

**NOTES**
- It moves in steps of the timebase's period, not of 1 ns.
- `planar/exec.h` says "since boot". It counts from the timebase's zero, which is at or before boot (`time_now` in `kernel/ktime.h`). Use differences, not the value.
- Every call traps. There is no clock read in user mode yet.

**BUGS**
None known.

**SEE ALSO**
[ArmTimer](#execarmtimer)

---

## exec/DeletePool

**NAME**
DeletePool -- free a pool and every block in it

**SYNOPSIS**
```c
#include <planar/exec.h>
void DeletePool(struct Pool *pool);
```

**FUNCTION**
Frees every Area the pool allocated: its puddles, and its blocks above the threshold, whether or not they were freed. The pool and every pointer into it are then invalid. A `CreatePoolMem` pool allocated none, so this does nothing to its buffer, which is the caller's to reuse.

A library function (`lib/exec/pool.c`). It traps once for each Area.

**INPUTS**
- `pool` -- from `CreatePool` or `CreatePoolMem`, or `nullptr`, which does nothing.

**NOTES**
- The quick way to free many blocks with one lifetime: allocate them all from one pool, and never call `FreePooled`.
- Don't delete a pool another task is still using, even one made with `POOLF_LOCKED`: the lock is in the pool.

**BUGS**
None known.

**SEE ALSO**
[CreatePool](#execcreatepool)

---

## exec/DeletePort

**NAME**
DeletePort -- remove a port

**SYNOPSIS**
```c
#include <planar/ipc.h>
int64_t DeletePort(Handle port);
```

**FUNCTION**
Removes the port's name, so new `OpenPort` calls fail with `ERR_NOTFOUND`. Connections still waiting to be accepted are refused: each is marked `CONNF_SERVER_GONE` and its client is signalled. Connections already accepted are not affected.

A kernel call.

**INPUTS**
- `port` -- a port handle owned by the calling process.

**RESULT**
0, or `ERR_BADHANDLE`.

**BUGS**
None known.

**SEE ALSO**
[CreatePort](#execcreateport)

---

## exec/DropInterface

**NAME**
DropInterface -- say an interface is no longer used

**SYNOPSIS**
```c
#include <planar/exec.h>
IExec->DropInterface(struct Interface *iface);
```

**FUNCTION**
Says the caller has done with an interface `GetInterface` returned. Call it once for each `GetInterface`, before `CloseLibrary`.

Through `IExec` only. It doesn't trap.

**INPUTS**
- `iface` -- the interface, or `nullptr`.

**RESULT**
None.

**NOTES**
- In V6 it does nothing, as `CloseLibrary` doesn't.

**BUGS**
None known.

**SEE ALSO**
[GetInterface](#execgetinterface), [CloseLibrary](#execcloselibrary)

---

## exec/ExitProcess

**NAME**
ExitProcess -- end the process

**SYNOPSIS**
```c
#include <planar/exec.h>
[[noreturn]] void ExitProcess(int64_t code);
```

**FUNCTION**
Ends every task in the calling process, then the process. Its handles, Areas, ports and connections go, and the other end of each connection is told. The processes it created are killed, and leases granted to it become stopping. Returning from `main()` calls this with `main`'s result.

A kernel call. Doesn't return.

**INPUTS**
- `code` -- the exit code. A process's creator reads it as `pi_ExitCode` with `QueryProcess`. For a process the kernel started, the kernel's test harness reports it.

**BUGS**
None known.

**SEE ALSO**
[ExitTask](#execexittask), [QueryProcess](#execqueryprocess)

---

## exec/ExitTask

**NAME**
ExitTask -- end the calling task

**SYNOPSIS**
```c
#include <planar/exec.h>
[[noreturn]] void ExitTask(int64_t code);
```

**FUNCTION**
Ends the calling task. Its ports and connections close and its stack is freed. If it was the process's last task, the process ends too.

A kernel call. Doesn't return.

**INPUTS**
- `code` -- currently ignored.

**BUGS**
`code` goes nowhere. There is no way yet to wait for a task to end, except by having it signal first.

**SEE ALSO**
[CreateTask](#execcreatetask), [ExitProcess](#execexitprocess)

---

## exec/FindTask

**NAME**
FindTask -- find a task

**SYNOPSIS**
```c
#include <planar/exec.h>
Handle FindTask(const char *name);
```

**FUNCTION**
With `name` = `nullptr`, returns the calling task's handle.

A kernel call.

**RESULT**
The handle, or `(Handle)ERR_NOTFOUND` for any non-null `name`.

**NOTES**
**Differs from Exec**: finding a task by name is **planned**, not built.

**BUGS**
The result type is `Handle`, unsigned 32-bit, so the error arrives as `(Handle)ERR_NOTFOUND` and `(int64_t)h > 0` is true for it. Compare with `(Handle)ERR_NOTFOUND`. The calls that make handles return `int64_t`; this one has not been changed yet.

**SEE ALSO**
[Signal](#execsignal), [CreateTask](#execcreatetask)

---

## exec/FreeArea

**NAME**
FreeArea -- give up an Area

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t FreeArea(Handle area);
```

**FUNCTION**
Frees the Area's handle and unmaps it from the calling process. If no other process maps it, the memory is freed. Otherwise it lasts until the last of them drops it.

A kernel call.

**INPUTS**
- `area` -- an Area the calling process owns.

**RESULT**
0, or `ERR_BADHANDLE` if `area` isn't an Area this process owns.

**NOTES**
- Freeing doesn't take the Area away from processes it was shared with. Unshare it from them first if that matters.
- Tasks of one process may race each other on the same handle. Of two `FreeArea` calls, one returns 0 and the other `ERR_BADHANDLE`; a `ShareArea` or `ProtectArea` that loses the race fails rather than map the Area again (`kernel/area.c`).

**BUGS**
None known.

**SEE ALSO**
[AllocArea](#execallocarea), [UnshareArea](#execunsharearea)

---

## exec/FreePooled

**NAME**
FreePooled -- give a block back to its pool

**SYNOPSIS**
```c
#include <planar/exec.h>
void FreePooled(struct Pool *pool, void *memory, uint64_t memSize);
```

**FUNCTION**
Puts the block on its size class's free list, for the pool's next block of that class. A block above the pool's threshold is an Area, and is freed.

A library function (`lib/exec/pool.c`). It traps only to free a block above the threshold.

**INPUTS**
- `pool` -- the pool the block came from.
- `memory` -- the block, or `nullptr`, which does nothing.
- `memSize` -- the size it was allocated with. Any size in the same class will do, but the same size is the rule.

**NOTES**
- The size is all the pool knows of a small block: there's no header. A wrong size puts the block on the wrong list, and a later block overlaps another. Use `AllocVec` where the size isn't to hand.
- A block above the threshold is looked up on the pool's list, so freeing one that isn't the pool's does nothing. A small block isn't checked.
- Puddles are never given back before `DeletePool`.

**BUGS**
None known.

**SEE ALSO**
[AllocPooled](#execallocpooled), [DeletePool](#execdeletepool)

---

## exec/FreeSignal

**NAME**
FreeSignal -- give back a signal bit

**SYNOPSIS**
```c
#include <planar/exec.h>
void FreeSignal(int64_t bit);
```

**FUNCTION**
Marks `bit` free in the calling task, so `AllocSignal` can hand it out again. Bits outside 16–63 are ignored.

A kernel call. **Built, untested**: no conformance program calls it.

**NOTES**
Doesn't clear the bit if it's set.

**BUGS**
None known.

**SEE ALSO**
[AllocSignal](#execallocsignal)

---

## exec/FreeVec

**NAME**
FreeVec -- free a block from AllocVec

**SYNOPSIS**
```c
#include <planar/exec.h>
void FreeVec(void *memoryBlock);
```

**FUNCTION**
Frees a block `AllocVec` returned, reading its size from the header before it. A block that is an Area of its own is freed with its Area.

A library function (`lib/exec/pool.c`).

**INPUTS**
- `memoryBlock` -- from `AllocVec`, or `nullptr`, which does nothing.

**NOTES**
Only `AllocVec`'s blocks: a pointer from anywhere else is read as if it had a header, and the pool is corrupted.

**BUGS**
None known.

**SEE ALSO**
[AllocVec](#execallocvec)

---

## exec/GetInterface

**NAME**
GetInterface -- one of a library's interfaces

**SYNOPSIS**
```c
#include <planar/exec.h>
struct Interface *IExec->GetInterface(struct Library *library, const char *name,
                                      uint32_t version);
```

**FUNCTION**
Returns `library`'s interface called `name`, if its version is `version` or later. An interface is a `struct Interface` header (its name, version, size and library), then the functions, in an order fixed for that name: a later version only adds at the end. Cast it to the library's type for it, such as `struct UtilityIFace`, and call through it.

Through `IExec` only. It doesn't trap.

**INPUTS**
- `library` -- from `OpenLibrary`.
- `name` -- the interface: every library has `"main"`.
- `version` -- the least version that will do.

**RESULT**
The interface, or `nullptr` if `library` has none by that name, or only an older one.

**EXAMPLE**
From `conformance/library.c`:
```c
struct Library *util = IExec->OpenLibrary("utility.library", 6);
struct UtilityIFace *IUtility =
    util ? (struct UtilityIFace *)IExec->GetInterface(util, "main", 1) : nullptr;

if (IUtility)
    IUtility->SNPrintf(line, sizeof line, "%s %lu", "via", 64UL);
```

**NOTES**
- `if_Size` is the table's size in bytes, header included: a program built against a later version can check that the entries it calls are there.
- The table is read-only, in every process: a write to it faults.

**BUGS**
None known.

**SEE ALSO**
[OpenLibrary](#execopenlibrary), [DropInterface](#execdropinterface)

---

## exec/KillProcess

**NAME**
KillProcess -- kill a process this process created

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t KillProcess(Handle process);
```

**FUNCTION**
Kills the process. Its tasks are told to stop, and it dies once each has left the kernel. A held process is discarded without running. Either way, the creating task's signal is set once the process is dead. The call doesn't wait for that.

The masking of interrupt lines comes first: a lease granted to the process has its lines masked before the call returns.

A kernel call.

**INPUTS**
- `process` -- a process handle owned by the calling process.

**RESULT**
0, or `ERR_BADHANDLE` if `process` isn't a process handle the calling process owns. A process that is already dead, or already ending, gets 0 and nothing happens.

**NOTES**
- `QueryProcess` then reports `PROCS_KILLED`, unless the process had already begun to exit or fault: its own ending stands.
- The handle stays valid. Close it with `CloseHandle` when done.
- The processes it created die with it, and leases granted to it become stopping.

**BUGS**
None known.

**SEE ALSO**
[CreateProcess](#execcreateprocess), [QueryProcess](#execqueryprocess), [CloseHandle](#execclosehandle)

---

## exec/KLog

**NAME**
KLog -- write a line to the kernel console

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t KLog(const char *text);
```

**FUNCTION**
Writes `[name] text` and a newline to the serial console, where `name` is the program's. Any byte that isn't printable ASCII (0x20–0x7E) is written as `?`, so a program can't move the cursor or clear the terminal.

The line is copied into the kernel log's ring, and the call returns: a kernel task shows it on the console later, in order with the kernel's own lines, writing only what the UART takes without waiting (V6 step 8). Only when the ring is full does the caller wait, for room. The ring keeps what it showed: the root's holder reads it back with `ReadLog`.

Each process may write 32 lines at once and 20 a second after that. Lines beyond that are dropped and counted. The next line that is written is preceded by `[name] (n lines dropped)`.

A kernel call.

**INPUTS**
- `text` -- a string. Only the first 200 characters are written.

**RESULT**
0, or `ERR_BADADDR` if `text` isn't readable. A dropped line also returns 0.

**NOTES**
- For bring-up and tests. A console device will replace it for programs.
- The limit is per process, shared by its tasks (`kernel/syscall.c`).

**BUGS**
- Truncation is silent, and so, until the next line, is dropping.
- A line in the ring when the system hangs with interrupts off isn't shown. A halt, a reboot and a meditation show what's in it first.

**SEE ALSO**
[ReadLog](lease.md#leasereadlog), [Libraries ch. 1](../libraries/01-introduction.md#logging)

---

## exec/OpenLibrary

**NAME**
OpenLibrary -- find a shared library

**SYNOPSIS**
```c
#include <planar/exec.h>
struct Library *IExec->OpenLibrary(const char *name, uint32_t version);
```

**FUNCTION**
Returns the base of the shared library called `name`, if its version is `version` or later. The base is a `struct Library`: its name, version and revision, an ID string, and its interfaces, which `GetInterface` returns.

The libraries are the ones in the boot image, relocated by the kernel at boot into the shared library region, which every process maps read and execute at the same address. `OpenLibrary` loads nothing: it looks in exec.library's directory, which the kernel filled in then.

Through `IExec` only. It doesn't trap. `IExec` is exec.library's main interface, which the kernel gives every process at its start (crt0 keeps it).

**INPUTS**
- `name` -- the library's name, such as `"utility.library"`.
- `version` -- the least version that will do.

**RESULT**
The base, or `nullptr` if there's no such library, or its version is older than `version`.

**NOTES**
- In V6, exec.library and utility.library are version 6 (revision 4). exec.library's own base is `IExec->Data.if_Library`.
- **Differs from Exec:** Exec's `OpenLibrary` found a library in memory or loaded it from `LIBS:`, and ran its init and open functions, which counted opens. Planar's libraries are code and constants only, in place since boot, so there's nothing to run or count.

**BUGS**
None known.

**SEE ALSO**
[GetInterface](#execgetinterface), [CloseLibrary](#execcloselibrary)

---

## exec/OpenPort

**NAME**
OpenPort -- connect to a port

**SYNOPSIS**
```c
#include <planar/ipc.h>
int64_t OpenPort(const char *name, int64_t sigbit, uint32_t slots, uint64_t heap,
                 struct ConnArena **arena, uint64_t deadline);
```

**FUNCTION**
Makes a connection to the port called `name`, waiting for it to be made if there's none yet and `deadline` hasn't passed (V6 step 7, VQ28). The wait ends when a task calls `CreatePort` with that name: no polling.
- allocates an arena Area, writes its header and two empty rings of `slots` entries, and maps it read-write into the calling process;
- queues the connection on the port and signals the port's owner.

From then on the calling task is the client end. When the server accepts, the arena appears in the server at the same address. Signal bit `sigbit` is set in the calling task each time the server calls `SignalConn` or `SignalWaitConn`, and when the server end closes.

A kernel call.

**INPUTS**
- `name` -- the port's name.
- `sigbit` -- 16–63.
- `slots` -- entries per ring: a power of two from 2 to `CONN_SLOTS_MAX` (4096), or 0 for `CONN_SLOTS_DEFAULT` (64).
- `heap` -- bytes for messages, up to `CONN_HEAP_MAX` (1 MiB), or 0 for `CONN_HEAP_DEFAULT` (4096). The arena is rounded up to whole pages, so the heap (`ca_HeapSize`) may be larger.
- `arena` -- where to store the arena's address.

**RESULT**
The connection's handle, or:
- `ERR_BADARG` -- `sigbit`, `slots` or `heap` out of range, or the port belongs to the calling process.
- `ERR_BADADDR` -- `name` or `arena` isn't accessible, or `name` is longer than 31 characters.
- `ERR_NOTFOUND` -- no port has that name, and `deadline` is 0; or the task was killed while it waited.
- `ERR_TIMEOUT` -- no port had that name by `deadline` (one already past included).
- `ERR_FULL` -- 32 connections are already waiting on the port to be accepted. Try again later. **Built, untested**: no conformance program fills a port's queue.
- `ERR_LIMIT` -- the calling process already holds 32 connection ends, or 256 handles.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.

**NOTES**
- The connection is usable before it's accepted: the client may fill the request ring at once.
- A connection waiting to be accepted counts against the client's 32 connection ends.
- If `arena` stops being writable during the call, the result is `ERR_BADADDR` but the connection exists. It's closed when the task ends.
- A waiter is listed while the name is missing, under the lock `CreatePort` takes, so a port made between the look and the sleep wakes it (`wait_name` in `kernel/port.c`). A port made and deleted before the woken waiter opens it is waited for again, until the deadline.
- Measured by `conformance/deadline.c`: a client waiting from before its server's `CreatePort` connects as it's made; a waiter killed in the wait is gone within a millisecond.

**BUGS**
In the `ERR_BADADDR` case the handle is lost to the caller.

**SEE ALSO**
[ConnectPort](msg.md#execconnectport), [AcceptConn](#execacceptconn), [CloseConn](#execcloseconn), `planar/ipc.h`

---

## exec/ProtectArea

**NAME**
ProtectArea -- change the owner's rights to an Area

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t ProtectArea(Handle area, uint32_t rights);
```

**FUNCTION**
Sets the calling process's rights to an Area it owns, and remaps it. With `AREAR_EXEC`, also synchronises the instruction cache over the whole Area.

A kernel call.

**INPUTS**
- `area` -- an Area the calling process owns.
- `rights` -- `AREAR_READ`, plus any of `AREAR_WRITE` and `AREAR_EXEC`.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- not an Area this process owns.
- `ERR_BADARG` -- an unknown bit, or no `AREAR_READ`.
- `ERR_RIGHTS` -- `AREAR_EXEC` without `AREAF_EXEC` or `AREAF_RWX`; or `AREAR_EXEC | AREAR_WRITE` without `AREAF_RWX`; or, without `AREAF_RWX`, `AREAR_WRITE` while another process maps the Area executable, or `AREAR_EXEC` while another maps it writable.
- `ERR_NOTFOUND` -- another task of the process freed the Area during the call.

**NOTES**
- Only the owner's own rights change. Processes it's shared with keep theirs.
- W^X holds over the whole Area, not each mapping: no process may write it while any process may execute it, unless it is `AREAF_RWX`.
- Other tasks of the process may go on using the Area during the call. One that touches it just as its mapping is replaced doesn't fault for it: the kernel retries the access. Once the call returns, the new rights hold on every CPU.

**BUGS**
None known.

**SEE ALSO**
[AllocArea](#execallocarea), [ShareArea](#execsharearea), [Libraries ch. 3](../libraries/03-areas.md)

---

## exec/QueryArea

**NAME**
QueryArea -- describe the Area an address lies in

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t QueryArea(const void *address, struct AreaInfo *info);
```

**FUNCTION**
Finds the Area mapped into the calling process that contains `address`, and fills in `info`:

| Field | |
|---|---|
| `ai_Address`, `ai_Size` | The whole Area |
| `ai_Area` | Its handle if the calling process owns it, otherwise 0 (including the program image and stacks, which the kernel owns) |
| `ai_Rights` | The calling process's rights |
| `ai_Flags` | The `AREAF_` flags it was made with |
| `ai_Sharers` | How many processes map it |

A kernel call.

**RESULT**
0, or:
- `ERR_NOTFOUND` -- no Area of the caller's contains `address`.
- `ERR_BADADDR` -- `info` isn't writable.

**BUGS**
None known.

**SEE ALSO**
[AllocArea](#execallocarea)

---

## exec/QueryProcess

**NAME**
QueryProcess -- say how a process is, or how it ended

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t QueryProcess(Handle process, struct ProcessInfo *info);
```

**FUNCTION**
Fills in `info` for a process the calling process created, or, with `process` 0, for the calling process itself. `pi_State` is one of:

| `pi_State` | |
|---|---|
| `PROCS_HELD` | Made with `PROCF_HELD` and not started |
| `PROCS_RUNNING` | Running, or ending but not yet dead |
| `PROCS_EXITED` | Dead after `ExitProcess`, a return from `main`, or its last task's `ExitTask` |
| `PROCS_FAULTED` | Dead after a fault |
| `PROCS_KILLED` | Dead after `KillProcess` or `CloseHandle` |

Once it is dead, the other fields say how:

| Field | |
|---|---|
| `pi_ExitCode` | The code given to `ExitProcess`, or `main`'s result. -1 after a fault. 0 if killed, or if its last task ended with `ExitTask` |
| `pi_Fault` | `PFAULT_NONE`, or which fault ended it |
| `pi_FaultAddress` | The address the fault was about: for an access fault, the one touched |
| `pi_FaultPC` | Where it faulted |

While the process is held or running, they are 0. Four are always filled in:
- `pi_Handles` -- the handles the process holds now, its tasks' among them. A supervisor checks with it that restarting its children leaks none (hwdb's kill test).
- `pi_Pages` -- the pages it maps, each Area once however it's shared: its image, stacks, Areas, and those shared with it. 0 once it's dead.
- `pi_LibraryPages` -- of those, the shared library region's, which is the same in every process, and in memory once ([Libraries ch. 8](../libraries/08-libraries.md)).
- `pi_Calls` -- the kernel calls its tasks have made since it started, the dead tasks' included. A program measures its own with `process` 0, as `conformance/window.c` counts a frame's (V7 step 3).

A kernel call.

**INPUTS**
- `process` -- a process handle owned by the calling process, or 0 for the calling process.
- `info` -- where to store the description.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `process` isn't 0 or a process handle the calling process owns.
- `ERR_BADADDR` -- `info` isn't writable.

**NOTES**
- A process is dead when the creating task's signal is set. Query it after that, not after `KillProcess` returns.
- The kernel log also prints a fault, with its kind, address and PC.

**BUGS**
None known.

**SEE ALSO**
[CreateProcess](#execcreateprocess), [KillProcess](#execkillprocess), [ExitProcess](#execexitprocess)

---

## exec/QueryTasks

**NAME**
QueryTasks -- say what a child process's tasks are doing

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t QueryTasks(Handle process, struct TaskMark *marks, uint32_t n);
```

**FUNCTION**
Fills in a `struct TaskMark` for each of the first `n` tasks of `process`, one the caller made, and returns how many tasks it has. For a supervisor to say where a server stalled before it restarts it, without a task watching the server: hwdb logs them for a server a client reported unresponsive (V6 step 8).

| Field | |
|---|---|
| `tm_State` | `TASKS_WAITING`, or `TASKS_RUNNING`: on a CPU or ready for one |
| `tm_Call` | The kernel call it's in, or made last (`SYS_*`, `planar/syscalls.h`) |
| `tm_InCall` | Nonzero: it's in `tm_Call` now |
| `tm_PC` | Where it made the call, as bytes from `tm_Image`: in the call's stub |
| `tm_Caller` | The stub's return address, from `tm_Image`: where in the program the call came from |
| `tm_Image` | Where the process's image is |
| `tm_Waiting` | The signals a waiting task waits for |

`tm_PC` and `tm_Caller`, as offsets, go straight to `llvm-addr2line -e` with the program's `.elf`.

A kernel call. Recording a call costs the kernel three stores on every call.

**INPUTS**
- `process` -- a process the calling process created.
- `marks` -- room for `n`.
- `n` -- at most this many are filled in; the kernel gives 16 at most.

**RESULT**
The number of tasks the process has (more than `n`, maybe), or:
- `ERR_BADHANDLE` -- `process` isn't one the caller holds.
- `ERR_BADADDR` -- `marks` isn't writable.

**NOTES**
- A task that has made no call yet has `tm_Call` 0 and `tm_PC` the image's start less its own.
- What a task is doing changes as the call returns: the marks are a moment's.

**BUGS**
None known.

**SEE ALSO**
[QueryProcess](#execqueryprocess), [ReportUnresponsive](msg.md#execreportunresponsive)

---

## exec/QuerySystem

**NAME**
QuerySystem -- the whole machine's counts since boot

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t QuerySystem(struct SystemInfo *info);
```

**FUNCTION**
Fills in `info` with counts summed over every CPU:

| Field | |
|---|---|
| `si_Calls` | Kernel calls made |
| `si_Interrupts` | Interrupts taken from the controller and the timer |
| `si_Switches` | Task switches |
| `si_Crossings` | Of those, from one process's domain to another's |
| `si_Quarantined` | DMA pages waiting for their device to stop (`RevokeLease`) |
| `si_IdleNanos` | Time every CPU has slept with nothing to run, added up (V8 step 7) |
| `si_CPUs` | CPUs online |
| `si_Pages`, `si_FreePages` | Memory's 4 KiB pages the kernel manages, and of those free now |

It's for measuring what an operation costs: read it before and after, and divide. `conformance/blkbench.c` reports calls, interrupts and crossings per I/O this way (docs §11.8). The Workbench's HUD reads it once a second: the CPU busy is 1 less the idle time over the time passed times `si_CPUs`, the memory used 1 less `si_FreePages` over `si_Pages`.

A kernel call.

**INPUTS**
- `info` -- where to store the counts.

**RESULT**
0, or `ERR_BADADDR` if `info` isn't writable.

**NOTES**
- The counts are the whole machine's, so other work shows in them. Measure with the machine otherwise quiet.
- Each CPU's counts are read without stopping it, so a count taken while others run is approximate.

**BUGS**
None known.

**SEE ALSO**
[QueryProcess](#execqueryprocess)

---

## exec/RawDoFmt

**NAME**
RawDoFmt -- format a string a byte at a time

**SYNOPSIS**
```c
#include <planar/exec.h>
typedef void (*PutChProc)(char ch, void *putChData);
uint64_t RawDoFmt(const char *formatString, va_list dataStream, PutChProc putChProc,
                  void *putChData);
```

**FUNCTION**
Formats `formatString` with the arguments in `dataStream`, calling `putChProc(ch, putChData)` for each byte, and once more with `'\0'` at the end. It allocates nothing, so it can write into a fixed buffer, a console, or a log line.

A conversion is `%`, then flags, a width, a precision, a length and a conversion character, as in C:
- **Flags:** `-` (left-align), `0` (pad with zeros), `+`, space, `#` (`0x`, `0b` or a leading `0`).
- **Width and precision:** digits, or `*` to take an `int` from the arguments. A negative `*` width means `-`.
- **Lengths:** none for `int`; `hh` and `h`; `l`, `ll`, `z`, `t` and `j`, all 64 bits; C23's `w8`, `w16`, `w32` and `w64`, and `wf` with the same widths.
- **Conversions:** `d`, `i`, `u`, `x`, `X`, `o`, `b` and `B` (binary, C23), `c`, `s`, `p` (`0x` and hex), `%`, and `S`, a `struct Str` by value.

A library function (`lib/exec/fmt.c`, in `libexec.a`). It doesn't trap.

**INPUTS**
- `formatString` -- the format.
- `dataStream` -- the arguments, from `va_start` in the caller. `RawDoFmt` reads a copy, so the caller's `va_list` is unchanged.
- `putChProc` -- called for each byte, then for the NUL.
- `putChData` -- passed to `putChProc`.

**RESULT**
The number of bytes written before the NUL.

**EXAMPLE**
A formatter into a buffer, from `conformance/fmtcheck.h` in outline:
```c
static void put(char c, void *data)
{
    struct buf *b = data;

    if (c && b->len < sizeof b->text - 1)
        b->text[b->len++] = c;
}

va_start(ap, fmt);
RawDoFmt(fmt, ap, put, &b);
va_end(ap);
```

**NOTES**
- **Differs from Exec:** Exec's `%d` was 16 bits and `%ld` 32, and its arguments were packed as words; these are C's sizes, and `%ld` is 64 bits on both ISAs. Exec returned the next argument; this returns the length.
- There's no floating point: `%f`, `%e`, `%g` and `%a` are written as they stand, as is any conversion it doesn't know.
- `%s` of `nullptr` writes `(null)`. `%S` writes exactly `st_Len` bytes, NULs and all: no NUL is needed.
- `SNPrintf`, over this, is utility.library's ([autodoc](utility.md#utilitysnprintf)). `struct Str` is in `planar/utility.h`.

**BUGS**
None known.

**SEE ALSO**
[KLog](#execklog)

---

## exec/ReleaseArea

**NAME**
ReleaseArea -- let go of an Area another process shared

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t ReleaseArea(const void *address);
```

**FUNCTION**
Unmaps from the calling process the Area containing `address`, which another process owns and shared with it (`ShareArea`), or did before it freed the Area or died. It's the sharer's side of `UnshareArea`: a server done with what a client lent it, such as `gpu-panthor` with an app's ring once its queue has gone, or intuition with a killed program's bitmaps. The Area itself goes only if nothing else holds it: its owner's handle and other mappings keep it.

A kernel call (53), added in V5 step 5: before it, only the owner could take a mapping back. Until V7 step 5 it also refused an Area whose owner had freed it or died, taking it for the kernel's, so what a dead client had lent a server still stayed mapped in the server until the server died. kill-window found it.

**RESULT**
0, or:
- `ERR_NOTFOUND` -- nothing the caller maps contains `address`.
- `ERR_RIGHTS` -- the caller owns that Area (free it with `FreeArea`), or the kernel made it (the program image, a stack, a connection's arena).

**NOTES**
- Tested by `conformance/conn.c`: the client lends the server an Area, the server releases it, and the client sees one sharer left; the refusals for the server's own Area and for the arena.
- A DMA Area of a lease that a peer maps (display's buffers) may be released by the peer as any other.

**BUGS**
None known.

**SEE ALSO**
[ShareArea](#execsharearea), [UnshareArea](#execunsharearea), [QueryArea](#execqueryarea)

---

## exec/SetSignal

**NAME**
SetSignal -- read and change this task's signals

**SYNOPSIS**
```c
#include <planar/exec.h>
uint64_t SetSignal(uint64_t bits, uint64_t mask);
```

**FUNCTION**
Sets the calling task's signals in `mask` to their values in `bits`, and leaves the others alone. Never sleeps.

A kernel call.

**INPUTS**
- `bits` -- new values.
- `mask` -- which signals to change. The kernel's own bits are removed from it.

**RESULT**
All the task's signals as they were before the call.

**NOTES**
`SetSignal(0, 0)` reads the signals. `SetSignal(0, m)` clears `m`.

**BUGS**
None known.

**SEE ALSO**
[Wait](#execwait), [Signal](#execsignal)

---

## exec/SetMem

**NAME**
SetMem -- fill memory with a byte

**SYNOPSIS**
```c
#include <planar/exec.h>
void *SetMem(void *dest, uint8_t fill, uint64_t size);
```

**FUNCTION**
Sets `size` bytes at `dest` to `fill`, and returns `dest`.

A library function, inline in `planar/exec.h`, over the runtime's `memset`. It doesn't trap. On both ISAs it stores 32 bytes or more a step with the vector unit.

**INPUTS**
- `dest` -- the memory, writable.
- `fill` -- the byte.
- `size` -- how many. 0 sets nothing.

**RESULT**
`dest`.

**NOTES**
Exec had no `SetMem`; this is AmigaOS 4's. A new Area is zeroed already: there's no need to clear it.

**BUGS**
None known.

**SEE ALSO**
[CopyMem](#execcopymem)

---

## exec/ShareArea

**NAME**
ShareArea -- map an Area into another process

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t ShareArea(Handle area, Handle task, uint32_t rights);
```

**FUNCTION**
Maps an Area the calling process owns into the process that `task` belongs to, at the same address, with `rights`. The target must be a process the caller has an open connection with, either way round.

A kernel call.

**INPUTS**
- `area` -- an Area the calling process owns.
- `task` -- any task of the target process.
- `rights` -- as for `ProtectArea`.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- not an Area this process owns, or another task of the process freed it during the call.
- `ERR_NOTFOUND` -- `task` isn't a task, or its process is ending.
- `ERR_RIGHTS` -- the target process isn't connected to the caller; or, as for `ProtectArea`, the rights aren't allowed, including W^X against the Area's other mappings.
- `ERR_BADARG` -- as for `ProtectArea`.
- `ERR_EXISTS` -- that process already maps it, the owner included.
- `ERR_NOMEM` -- out of memory.

**NOTES**
- The target process isn't told. Send it the address in a message.
- **Connected** means a connection both ends of which are open: accepted, and not yet closed by either side. A connection still waiting to be accepted doesn't count (`ipc_connected` in `kernel/port.c`).
- The check is made only when sharing. The mapping stays when the connection closes; use `UnshareArea` to take it back.
- The caller gets the target's task handle from the target, in a message (`FindTask(nullptr)` there), as `conformance/conn.c` does.

**BUGS**
None known.

**SEE ALSO**
[UnshareArea](#execunsharearea), [ReleaseArea](#execreleasearea), [Libraries ch. 3](../libraries/03-areas.md)

---

## exec/Signal

**NAME**
Signal -- send signals to a task

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t Signal(Handle task, uint64_t mask);
```

**FUNCTION**
Sets the bits of `mask` in `task`'s signals, waking it if it's waiting for any of them. If it wakes on another CPU, that CPU is interrupted.

A kernel call.

**INPUTS**
- `task` -- a task in the calling process, the caller included.
- `mask` -- the signals. The kernel's own bits are removed.

**RESULT**
0, or `ERR_BADHANDLE` if `task` isn't a task, or is a task of another process.

**NOTES**
**Differs from Exec**: Exec's `Signal` reaches any task. Planar's stays inside the process. To wake a task in another process, use a connection: `SignalConn` reaches only its peer (`kernel/syscall.c`).

**BUGS**
None known.

**SEE ALSO**
[Wait](#execwait), [SetSignal](#execsetsignal)

---

## exec/SignalConn

**NAME**
SignalConn -- wake the other end of a connection

**SYNOPSIS**
```c
#include <planar/ipc.h>
int64_t SignalConn(Handle conn);
```

**FUNCTION**
Sets the signal bit the other end chose for this connection, in the other end's task. The rings are the caller's business; the kernel doesn't look at them.

It also ends any priority this task was lent by a client's `SignalWaitConn` (handoff boosting).

A kernel call. `PutMsg` and `ReplyMsg` make it only when the other end has said it's asleep.

**INPUTS**
- `conn` -- a connection handle owned by the calling process.

**RESULT**
0, or:
- `ERR_BADHANDLE`.
- `ERR_PORT_DIED` -- the other end has closed. (A client's connection not yet accepted returns 0.)

**BUGS**
None known.

**SEE ALSO**
[SignalWaitConn](#execsignalwaitconn), [PutMsg](msg.md#execputmsg)

---

## exec/SignalWaitConn

**NAME**
SignalWaitConn -- wake the other end and wait, in one call

**SYNOPSIS**
```c
#include <planar/ipc.h>
uint64_t SignalWaitConn(Handle conn, uint64_t mask, uint64_t deadline);
```

**FUNCTION**
`SignalConn(conn)` then `WaitUntil(mask, deadline)`, with two differences:
- If the other end was waiting and may run on this CPU, it runs next here: the CPU is handed straight to it.
- From the client end, a server handed the CPU this way runs at no less than the client's priority until it next signals or waits.

A kernel call. `DoMsg`, `DoMsgUntil` and `WaitPorts` use it.

**INPUTS**
- `conn` -- a connection handle owned by the calling process.
- `mask` -- the signals to wait for.
- `deadline` -- when to give up, as for `WaitUntil`; `DEADLINE_NONE` never (V6 step 7).

**RESULT**
The signals in `mask` that ended the wait, as for `Wait`; 0 at the deadline. 0, without waiting, if the other end has closed or `conn` isn't a connection handle.

**NOTES**
The loan is handoff boosting, not full priority inheritance ([docs §4.4](../../docs/04-ipc.md)). A server that was already running or ready is lent nothing; a loan isn't passed on when the server waits on a server of its own; and it ends at the server's next signal or wait, not when the request is answered (`task_post` in `kernel/sched.c`).

**BUGS**
A bad handle and a closed peer both return 0 at once, which is also `WaitPorts`'s "look again". Callers check `PortGone`.

**SEE ALSO**
[DoMsg](msg.md#execdomsg), [WaitPorts](msg.md#execwaitports), [Wait](#execwait)

---

## exec/StartProcess

**NAME**
StartProcess -- start a held process

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t StartProcess(Handle process);
```

**FUNCTION**
Starts a process made with `PROCF_HELD`: its first task runs the program's `main`.

A kernel call.

**INPUTS**
- `process` -- a process handle owned by the calling process.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `process` isn't a process handle the calling process owns.
- `ERR_EXISTS` -- the process isn't held: it was started already, or was killed while held.

**NOTES**
- Holding a process is for granting it what it needs, a lease, before it runs.
- If another task kills the process while this call starts it, the call still returns 0, and the process dies without running (`process_start_call` in `kernel/process.c`).

**BUGS**
None known.

**SEE ALSO**
[CreateProcess](#execcreateprocess), [GrantLease](lease.md#leasegrantlease)

---

## exec/UnshareArea

**NAME**
UnshareArea -- unmap an Area from another process

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t UnshareArea(Handle area, Handle task);
```

**FUNCTION**
Unmaps an Area the calling process owns from the process that `task` belongs to.

A kernel call. **Built, untested**: no conformance program calls it.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- not an Area this process owns.
- `ERR_NOTFOUND` -- `task` isn't a task, belongs to the owner, or its process doesn't map the Area.

**NOTES**
- The other process gets no warning. Its next access to the Area faults and ends it, so agree on a protocol first.
- Unlike `ShareArea`, this needs no connection: an Area can be taken back after the connection has closed.

**BUGS**
None known.

**SEE ALSO**
[ShareArea](#execsharearea), [FreeArea](#execfreearea)

---

## exec/Wait

**NAME**
Wait -- wait for signals

**SYNOPSIS**
```c
#include <planar/exec.h>
uint64_t Wait(uint64_t mask);
```

**FUNCTION**
If any signal in `mask` is set, clears those and returns them. Otherwise sleeps until one is.

A kernel call.

**INPUTS**
- `mask` -- the signals to wait for. The kernel's own bits are removed.

**RESULT**
The signals in `mask` that were set. They're cleared.

**NOTES**
- `Wait(0)` sleeps until the process is killed.
- Ending a wait also ends any priority this task was lent by a client's `SignalWaitConn` (see there).

**BUGS**
None known.

**SEE ALSO**
[Signal](#execsignal), [SetSignal](#execsetsignal), [SignalWaitConn](#execsignalwaitconn)

---

## exec/WaitAddr

**NAME**
WaitAddr -- sleep while a word in memory holds a value

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t WaitAddr(const volatile uint64_t *address, uint64_t value, uint64_t deadline);
```

**FUNCTION**
Sleeps while the 64-bit word at `address` holds `value`. The wait ends when `WakeAddr` is called on the word, the deadline passes, or the task is killed. The call returns at once if the word holds something else.

Words are matched by their physical address, so the waiter and the waker may be in different processes, each mapping the word where it is (the identity layout). The kernel lists the waiter before it reads the word, so a `WakeAddr` that comes after the word is changed is never missed.

A kernel call.

**INPUTS**
- `address` -- the word: 8-byte aligned, in memory the caller maps readable.
- `value` -- what the word held when the caller last looked.
- `deadline` -- when to give up, in nanoseconds on `CurrentTime`'s clock; 0 for no deadline.

**RESULT**
0 when woken, or when the word didn't hold `value`. Otherwise:
- `ERR_TIMEOUT` -- the deadline passed with the word unchanged.
- `ERR_BADADDR` -- `address` isn't aligned or isn't mapped.
- `ERR_NOSYS` -- a deadline, and the kernel found no timer interrupt.

**EXAMPLE**
Waiting for a sequence number to reach `want`, from `conformance/futex.c`:
```c
uint64_t v;

while ((v = __atomic_load_n(seq, __ATOMIC_ACQUIRE)) != want)
    if (WaitAddr(seq, v, CurrentTime() + 2000 * MS) == ERR_TIMEOUT)
        return false;
```

**NOTES**
- A return of 0 says nothing about the word: look at it again. Waits end early, for instance when any mapping of the word's Area goes. That includes the death of a process that shared it, which may have been the word's only writer.
- For sequence numbers and other single-writer words only, never as a lock between processes ([docs §12.5](../../docs/12-let-it-crash.md)). A holder that died would leave the others waiting.
- The word is read through the caller's own mapping, so it may be in an `AREAF_NC` Area that a device writes.
- A killed task's wait ends, and the task stops on its way back to user mode.
- On QEMU's TCG, a sequence number passed back and forth between two processes takes 35–50 µs a round trip (`conformance/futex.c` logs it).

**BUGS**
None known.

**SEE ALSO**
[WakeAddr](#execwakeaddr), [Wait](#execwait), [CurrentTime](#execcurrenttime)

---

## exec/WaitUntil

**NAME**
WaitUntil -- wait for signals, until a deadline

**SYNOPSIS**
```c
#include <planar/exec.h>
uint64_t WaitUntil(uint64_t mask, uint64_t deadline);
```

**FUNCTION**
`Wait`, giving up at `deadline` (V6 step 7). A deadline already past only takes the signals there are, without sleeping: `WaitUntil(mask, 0)` is a poll. Its alarm is the kernel's own, not the task's `ArmTimer`'s, which stays as it was.

A kernel call. exec.library's interface version 2 adds it (`IExec->WaitUntil`).

**INPUTS**
- `mask` -- the signals to wait for. The kernel's own bits are removed.
- `deadline` -- on `CurrentTime`'s clock; `DEADLINE_NONE` (`planar/syscalls.h`) never comes.

**RESULT**
The signals in `mask` that were set, cleared; 0 if none was by `deadline`.

**NOTES**
- Without a timer interrupt (`ArmTimer`'s `ERR_NOSYS`) there's nothing to end the wait, and a deadline in the future is waited as `Wait` waits.
- `DoMsgUntil` sleeps with it (`task_wait_until` in `kernel/sched.c`).

**BUGS**
None known.

**SEE ALSO**
[Wait](#execwait), [ArmTimer](#execarmtimer), [DoMsgUntil](msg.md#execdomsguntil)

---

## exec/WakeAddr

**NAME**
WakeAddr -- wake tasks waiting on a word

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t WakeAddr(const volatile uint64_t *address, uint32_t count);
```

**FUNCTION**
Wakes up to `count` of the tasks sleeping in `WaitAddr` on the word at `address`, in any process. Change the word first, then wake.

A kernel call.

**INPUTS**
- `address` -- the word: 8-byte aligned, in memory the caller maps.
- `count` -- the most to wake; `~0u` for all.

**RESULT**
How many it woke (0 if none were waiting), or:
- `ERR_BADADDR` -- `address` isn't aligned or isn't mapped.

**EXAMPLE**
```c
__atomic_store_n(seq, next, __ATOMIC_RELEASE);
WakeAddr(seq, 1);
```

**NOTES**
- The call always traps. A writer that wants to skip it when nobody sleeps keeps its own flag, as connection rings do with `cr_Waiting` ([docs §4.3](../../docs/04-ipc.md)).

**BUGS**
None known.

**SEE ALSO**
[WaitAddr](#execwaitaddr)
