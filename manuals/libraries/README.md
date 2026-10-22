# Libraries

*Planar Reference Manuals, volume 1.* How to write programs for Planar. The chapters teach; the [Autodocs](../autodocs/README.md) are the reference.

## Chapters

| # | Chapter | State |
|---|---|---|
| 1 | [Introduction to Planar programming](01-introduction.md): the single address space, processes and tasks, handles, kernel calls and errors | Written, V4; checked V5 |
| 2 | [Exec: tasks and signals](02-tasks-and-signals.md) | Written, V4; checked V5 |
| 3 | [Exec: memory and Areas](03-areas.md) | Written, V4; `AREAF_NC` and `ReleaseArea`, V5 |
| 4 | [Exec: ports, connections and messages](04-messages.md) | Written, V4; checked V5 |
| 5 | Exec: interrupts and driver tasks | Built in V4 as leases' lines (`BindIrq`, `AckWaitIrq`): see [Devices ch. 1](../devices/01-drivers.md) |
| 6 | Exec: lending memory with a request (`IOF_LEND`) | **Planned** ([docs §4.2](../../docs/04-ipc.md)). Meanwhile the block class passes buffers in the connection's heap ([Devices ch. 2](../devices/02-block.md)) |
| 7 | [Exec: futexes](07-futexes.md) (`WaitAddr`, `WakeAddr`) | Written, V5 |
| 8 | Libraries and interface tables (`IExec->PutMsg()`) | **Planned** ([docs §7.2](../../docs/07-language-toolchain.md)) |
| 9 | [Queue leases: GPU work from user space](09-queue-leases.md) | Written, V5. The NPU's is V7 |
| 10 | The hardware database (`hwdb`) | Built in V4: see [Devices ch. 1](../devices/01-drivers.md) ([docs §9](../../docs/09-hardware-description.md)) |

Where the RKM had chapters on Intuition, graphics and layers, Planar has none yet. They will be added here as those libraries are designed.

## Where Planar's Exec stands

Today "Exec" means two things a program links against:
- **The kernel calls** in `include/planar/exec.h` and `include/planar/ipc.h`, one stub each in `arch/<isa>/user/syscalls.S`.
- **`lib/exec`**, linked into every user program, which builds Exec's message-port API (`include/planar/msg.h`) on top of connections.

`exec.library`, reached through an interface table, is **planned** and will wrap both. Until then programs call the functions directly. Code written that way will keep compiling: the interface table adds `IExec->`, it doesn't rename anything.
