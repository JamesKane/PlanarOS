# 2. Exec: tasks and signals

*Checked against V5.*

Tasks are how a Planar program does several things at once. Signals are how a task sleeps until something happens, and how other tasks, the kernel and the IPC library wake it. Every way of blocking in Planar comes down to `Wait()` on a signal mask.

## Concepts

### Tasks

- A process starts with one task, which runs `main()`.
- `CreateTask(entry, arg)` starts another in the same process: same memory, same handles, its own 16 KiB stack. It returns the new task's handle, or a negative error. A process can have 64 tasks at once; past that, `ERR_LIMIT`.
- A task ends by calling `ExitTask(code)`, or by returning from its entry function, which is the same as `ExitTask(0)`.
- **`thread_local` variables are per task.** Each task starts with its own copy, at the values the program gave them, or zero. Writing one changes only the calling task's copy. The copy is at the top of the task's stack, so a large one comes out of the 16 KiB.
- When the last task of a process ends, the process ends. `ExitProcess(code)` ends every task at once.
- `FindTask(nullptr)` returns the calling task's handle.

A task runs at its process's priority. The scheduler runs tasks on any CPU, with per-CPU run queues and a 4 ms quantum. A new task goes to an idle CPU, the fastest first: on a board with big and little cores, the big ones fill first and the work spills onto the little ones. When no CPU is idle, it goes where it gets the most capacity per task. A task that waits and wakes stays on its CPU, unless an idle one takes it. There is no call yet to change a task's priority or to pin it to a CPU.

### Signals

Each task has **64 signal bits**:

| Bits | Owner |
|---|---|
| 0–15 | The system. The kernel keeps a few for itself (currently bits 2 and 3); user calls can't wait for, send or change those, and the bits are silently dropped from any mask. Bit 12 is `SIGBREAKF_CTRL_C`. |
| 16–63 | The task. Handed out by `AllocSignal`. |

A signal is a bit, not a count. Signalling a bit that is already set does nothing more. Two signals before the task wakes look like one.

The calls:
- `Signal(task, mask)` sets bits in another task of the same process (or this one). If the task is waiting for any of them, it wakes. A task in another process is out of reach: `Signal` returns `ERR_BADHANDLE`. Wake it over a connection instead (chapter 4).
- `Wait(mask)` returns at once if any bit in `mask` is set. Otherwise it sleeps until one is. It returns the bits of `mask` that were set, and clears them.
- `SetSignal(bits, mask)` reads this task's signals and changes the ones in `mask` without sleeping. `SetSignal(0, 0)` just reads them.
- `AllocSignal(-1)` takes the lowest free bit from 16–63, clears it, and returns its number. `AllocSignal(n)` takes bit `n`. `FreeSignal(n)` gives it back.

### Time

- `CurrentTime()` is nanoseconds since boot.
- `ArmTimer(deadline, sigbit)` is the calling task's **one alarm**: it sets `sigbit` once `CurrentTime()` reaches `deadline`. The deadline is absolute. Arming again replaces the alarm, and a deadline of 0 disarms it.

So a timeout is a signal like any other: wait for the alarm's bit along with the bits you're waiting for, and see which came.

## Using it

### Waiting for workers

From `conformance/tasks.c`: the main task starts three workers, each of which signals it and ends.

```c
static Handle main_task;

static void worker(uint64_t n)
{
    Signal(main_task, 1ULL << (20 + n));
    ExitTask(0);
}

int main(void)
{
    uint64_t want = 0, got = 0;

    main_task = FindTask(nullptr);
    for (int n = 0; n < 3; n++) {
        want |= 1ULL << (20 + n);
        if (CreateTask(worker, (uint64_t)n) <= 0)
            return 1;
    }
    while (got != want)
        got |= Wait(want);
    return 0;
}
```

`Wait` may return with only some of the bits, so the loop collects them. A real program should use bits from `AllocSignal` rather than fixed numbers.

### Checking for a break without sleeping

From `conformance/talker.c`: keep working until someone sends Ctrl-C.

```c
while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C))
    do_one_round();
```

To consume the break as well, use `SetSignal(0, SIGBREAKF_CTRL_C)`: it clears the bit and returns the old signals.

### Sleeping

From `conformance/dostest.c`:

```c
static void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_NAP);
    Wait(1ULL << SIG_NAP);
}
```

For a wait with a timeout, arm the alarm, `Wait` for both bits, and disarm it with `ArmTimer(0, 0)` if the other came first. An alarm that fired meanwhile leaves its bit set: clear it with `SetSignal(0, 1ULL << bit)` before the next wait.

## Rules

- **Allocate the bits you use.** Libraries allocate theirs with `AllocSignal(-1)` too, so a fixed bit number can collide with one a library is using.
- **Signals belong to tasks, not processes.** A bit allocated in one task means nothing in another. Waking "the process" means signalling a particular task.
- **`Signal` stays inside the process.** Another process's task handle gets `ERR_BADHANDLE`. Between processes, use a connection.
- **The alarm is the task's, not the library's.** A library that arms it takes it from its caller, so a task that has its own alarm armed shouldn't call one that sleeps.
- **Wait for every bit you might need.** A signal that arrives while you're waiting for something else stays set until you look for it.
- **`Wait(0)` never returns** unless the process is killed.
- **Signals carry no data.** To send data, send a message (chapter 4) and use the signal only to say one is there.

## Differences from Exec

| Exec | Planar |
|---|---|
| 32 signals; 0–15 the system's | 64 signals; 0–15 the system's |
| `FindTask(name)` finds any task by name | Only `FindTask(nullptr)` works; a name returns an error (**Planned**: a task name table) |
| `Signal(struct Task *, …)` to any task | `Signal(Handle, …)`, only to a task in the caller's own process; across processes, `SignalConn` to a connection's peer |
| `AllocSignal` returns -1 when none is free | The same |
| `timer.device`, with an `IORequest` per delay | `ArmTimer`, one alarm per task, which sets a signal bit |
| A task's data is reached through `tc_UserData` | `thread_local`, a copy per task, in C23's own syntax |
| `SetTaskPri`, `Forbid`, `Disable` | None. There is no way to stop other tasks running: use a message, or wait on a sequence number (chapter 7). |

## Functions used

[AllocSignal](../autodocs/exec.md#execallocsignal),
[ArmTimer](../autodocs/exec.md#execarmtimer),
[CreateTask](../autodocs/exec.md#execcreatetask),
[CurrentTime](../autodocs/exec.md#execcurrenttime),
[ExitProcess](../autodocs/exec.md#execexitprocess),
[ExitTask](../autodocs/exec.md#execexittask),
[FindTask](../autodocs/exec.md#execfindtask),
[FreeSignal](../autodocs/exec.md#execfreesignal),
[SetSignal](../autodocs/exec.md#execsetsignal),
[Signal](../autodocs/exec.md#execsignal),
[SignalConn](../autodocs/exec.md#execsignalconn),
[Wait](../autodocs/exec.md#execwait).
