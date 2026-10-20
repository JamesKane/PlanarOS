# Libraries

*Planar Reference Manuals, volume 1.* How to write programs for Planar. The chapters teach; the [Autodocs](../autodocs/README.md) are the reference.

## Chapters

| # | Chapter | State |
|---|---|---|
| 1 | [Introduction to Planar programming](01-introduction.md): the single address space, processes and tasks, handles, kernel calls and errors | Written, V3 |
| 2 | [Exec: tasks and signals](02-tasks-and-signals.md) | Written, V3 |
| 3 | [Exec: memory and Areas](03-areas.md) | Written, V3 |
| 4 | [Exec: ports, connections and messages](04-messages.md) | Written, V3 |
| 5 | Exec: interrupts and driver tasks (`AckIrq`) | **Planned**, V4 ([docs §4.6](../../docs/04-ipc.md)) |
| 6 | Exec: lending memory with a request (`IOF_LEND`) | **Planned** ([docs §4.2](../../docs/04-ipc.md)) |
| 7 | Exec: futexes (`WaitAddr`, `WakeAddr`) | **Planned** ([docs §4.1](../../docs/04-ipc.md)) |
| 8 | Libraries and interface tables (`IExec->PutMsg()`) | **Planned** ([docs §7.2](../../docs/07-language-toolchain.md)) |
| 9 | Queue leases: GPU and NPU from user space | **Planned**, V5 ([docs §5](../../docs/05-coprocessors.md)) |
| 10 | The hardware database (`hwdb`) | **Planned**, V4 ([docs §9](../../docs/09-hardware-description.md)) |

Where the RKM had chapters on Intuition, graphics and layers, Planar has none yet. They will be added here as those libraries are designed.

## Where Planar's Exec stands

Today "Exec" means two things a program links against:
- **The kernel calls** in `include/planar/exec.h` and `include/planar/ipc.h`, one stub each in `arch/<isa>/user/syscalls.S`.
- **`lib/exec`**, linked into every user program, which builds Exec's message-port API (`include/planar/msg.h`) on top of connections.

`exec.library`, reached through an interface table, is **planned** and will wrap both. Until then programs call the functions directly. Code written that way will keep compiling: the interface table adds `IExec->`, it doesn't rename anything.
