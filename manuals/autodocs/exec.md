# exec: kernel calls

*Checked against V2.*

The kernel calls, in alphabetical order. Each is one stub in `arch/<isa>/user/syscalls.S` and traps on every call. The headers are `planar/exec.h` (tasks, signals, Areas) and `planar/ipc.h` (ports and connections). Errors are listed in [Libraries ch. 1](../libraries/01-introduction.md#kernel-calls-and-errors), and the per-process limits behind `ERR_LIMIT` [there too](../libraries/01-introduction.md#limits).

Most programs use connections through the [msg library](msg.md) rather than the connection calls here.

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
- `flags` -- any of `AREAF_CLEAR`, `AREAF_CONTIG`, `AREAF_EXEC`, `AREAF_RWX`, `AREAF_LARGE`. See [Libraries ch. 3](../libraries/03-areas.md#flags).
- `address` -- where to store the Area's address.

**RESULT**
The Area's handle (positive), or:
- `ERR_BADARG` -- `size` is 0 or more than half the user address window, or `flags` has an unknown bit.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full. A `size` of more pages than are free fails at once, before anything is allocated.
- `ERR_LIMIT` -- the calling process already holds 256 handles.
- `ERR_BADADDR` -- `address` isn't writable. The Area has been freed again.
- `ERR_BADHANDLE` -- another task of the process freed the new handle before this call returned.

**NOTES**
- The Area starts with `AREAR_READ | AREAR_WRITE` rights, even with `AREAF_EXEC`. Use `ProtectArea` to make it executable.
- `AREAF_CLEAR` changes nothing: every Area is zeroed.
- Keep the result in an `int64_t` and test it with `< 0`; cast to `Handle` only once it's known to be one.

**BUGS**
None known.

**SEE ALSO**
[FreeArea](#execfreearea), [ProtectArea](#execprotectarea), [QueryArea](#execqueryarea), [ShareArea](#execsharearea)

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

A kernel call.

**INPUTS**
- `name` -- 1 to 31 characters.
- `sigbit` -- 16–63.

**RESULT**
The port's handle, or:
- `ERR_BADARG` -- `sigbit` is out of range.
- `ERR_BADADDR` -- `name` isn't readable, is empty, or is longer than 31 characters.
- `ERR_EXISTS` -- a port with that name exists.
- `ERR_LIMIT` -- the calling process already holds 256 handles.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.

**NOTES**
The port belongs to the calling task and goes when it ends.

**BUGS**
An over-long name gives `ERR_BADADDR`, where `ERR_BADARG` would be clearer.

**SEE ALSO**
[DeletePort](#execdeleteport), [AcceptConn](#execacceptconn), [OpenPort](#execopenport)

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
Starts a task in the calling process, running `entry(arg)` on a new 16 KiB stack at the process's priority. The task may start on any CPU, before this call returns.

A kernel call.

**INPUTS**
- `entry` -- the function to run. It must end with `ExitTask` and must not return.
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

**BUGS**
- Returning from `entry` isn't caught; there is no return address to go to.

**SEE ALSO**
[ExitTask](#execexittask), [FindTask](#execfindtask), [Libraries ch. 2](../libraries/02-tasks-and-signals.md)

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

## exec/ExitProcess

**NAME**
ExitProcess -- end the process

**SYNOPSIS**
```c
#include <planar/exec.h>
[[noreturn]] void ExitProcess(int64_t code);
```

**FUNCTION**
Ends every task in the calling process, then the process. Its handles, Areas, ports and connections go, and the other end of each connection is told. Returning from `main()` calls this with `main`'s result.

A kernel call. Doesn't return.

**INPUTS**
- `code` -- the exit code, reported to whatever started the process (today, the kernel's test harness).

**BUGS**
None known.

**SEE ALSO**
[ExitTask](#execexittask)

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
- The console is still polled: a long line holds up the CPU, with interrupts off, for about 19 ms at 115200 baud. A buffered console drained by its driver is **planned** with the drivers (V4).

**SEE ALSO**
[Libraries ch. 1](../libraries/01-introduction.md#logging)

---

## exec/OpenPort

**NAME**
OpenPort -- connect to a port

**SYNOPSIS**
```c
#include <planar/ipc.h>
int64_t OpenPort(const char *name, int64_t sigbit, uint32_t slots, uint64_t heap,
                 struct ConnArena **arena);
```

**FUNCTION**
Makes a connection to the port called `name`:
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
- `ERR_NOTFOUND` -- no port has that name.
- `ERR_FULL` -- 32 connections are already waiting on the port to be accepted. Try again later. **Built, untested**: no conformance program fills a port's queue.
- `ERR_LIMIT` -- the calling process already holds 32 connection ends, or 256 handles.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.

**NOTES**
- The connection is usable before it's accepted: the client may fill the request ring at once.
- A connection waiting to be accepted counts against the client's 32 connection ends.
- If `arena` stops being writable during the call, the result is `ERR_BADADDR` but the connection exists. It's closed when the task ends.

**BUGS**
In that last case the handle is lost to the caller.

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
[UnshareArea](#execunsharearea), [Libraries ch. 3](../libraries/03-areas.md)

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
uint64_t SignalWaitConn(Handle conn, uint64_t mask);
```

**FUNCTION**
`SignalConn(conn)` then `Wait(mask)`, with two differences:
- If the other end was waiting and may run on this CPU, it runs next here: the CPU is handed straight to it.
- From the client end, a server handed the CPU this way runs at no less than the client's priority until it next signals or waits.

A kernel call. `DoMsg` and `WaitPorts` use it.

**INPUTS**
- `conn` -- a connection handle owned by the calling process.
- `mask` -- the signals to wait for.

**RESULT**
The signals in `mask` that ended the wait, as for `Wait`. 0, without waiting, if the other end has closed or `conn` isn't a connection handle.

**NOTES**
The loan is handoff boosting, not full priority inheritance ([docs §4.4](../../docs/04-ipc.md)). A server that was already running or ready is lent nothing; a loan isn't passed on when the server waits on a server of its own; and it ends at the server's next signal or wait, not when the request is answered (`task_post` in `kernel/sched.c`).

**BUGS**
A bad handle and a closed peer both return 0 at once, which is also `WaitPorts`'s "look again". Callers check `PortGone`.

**SEE ALSO**
[DoMsg](msg.md#execdomsg), [WaitPorts](msg.md#execwaitports), [Wait](#execwait)

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
