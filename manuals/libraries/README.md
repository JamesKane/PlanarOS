# Libraries

*Planar Reference Manuals, volume 1.* How to write programs for Planar. The chapters teach; the [Autodocs](../autodocs/README.md) are the reference.

## Chapters

| # | Chapter | State |
|---|---|---|
| 1 | [Introduction to Planar programming](01-introduction.md): the single address space, processes and tasks, handles, kernel calls and errors | Written, V4; checked V5 |
| 2 | [Exec: tasks and signals](02-tasks-and-signals.md) | Written, V4; checked V5 |
| 3 | [Exec: memory and Areas](03-areas.md) | Written, V4; `AREAF_NC` and `ReleaseArea`, V5; pools and `AllocVec`, V6 |
| 4 | [Exec: ports, connections and messages](04-messages.md) | Written, V4; checked V5 |
| 5 | [utility.library](05-utility.md): tag lists, hooks, strings, `Span` and `Str` | Written, V6 |
| 6 | Exec: lending memory with a request (`IOF_LEND`) | **Planned** ([docs §4.2](../../docs/04-ipc.md)). Meanwhile the block class passes buffers in the connection's heap ([Devices ch. 2](../devices/02-block.md)) |
| 7 | [Exec: futexes](07-futexes.md) (`WaitAddr`, `WakeAddr`) | Written, V5 |
| 8 | [Libraries and interfaces](08-libraries.md): the shared library region, `OpenLibrary`, `GetInterface`, `IExec` | Written, V6 |
| 9 | [Queue leases: GPU work from user space](09-queue-leases.md) | Written, V5. The NPU's is V9 |
| 10 | The hardware database (`hwdb`) | Built in V4: see [Devices ch. 1](../devices/01-drivers.md) ([docs §9](../../docs/09-hardware-description.md)) |
| 11 | [C's library](11-clib.md) (`lib/clib`): `malloc`, the string functions, `strtol`, `qsort`, `snprintf`, `stdbit.h`, for code written for other systems | Written, V6 |
| 12 | [gfx.library](12-gfx.md): bitmaps, rastports, `RectFill`, `BltBitMap`, bevels; paths, gradients and type, blended in linear light; on the CPU | Written, V7; paths and type, V8 |
| 13 | [intuition](13-intuition.md): windows on the screen, from the program's own bitmaps, presented and composed; their chrome, focus and events; programs and servers dying | Written, V7 steps 3 to 5 |
| 14 | [font.library](14-font.md): TrueType fonts read in place, the system's two, text shaped and broken into lines | Written, V8 |

The RKM's Exec chapter on interrupts has no counterpart here: a driver's interrupts are its lease's lines (`BindIrq`, `AckWaitIrq`), in [Devices ch. 1](../devices/01-drivers.md).

Where the RKM had chapters on graphics, chapter 12 is gfx.library's, chapter 13 is intuition's, and chapter 14 font.library's, where the RKM had diskfont.library's. Layers have no counterpart: intuition composes windows, so none draws over another's.

## Where Planar's Exec stands

Today "Exec" means three things:
- **The kernel calls** in `include/planar/exec.h` and `include/planar/ipc.h`, one stub each in `arch/<isa>/user/syscalls.S`.
- **`lib/exec`**, linked into every user program, which builds Exec's message-port API (`include/planar/msg.h`) on top of connections, and the pools, `AllocVec` and `RawDoFmt` (`libexec.a`).
- **exec.library** (V6), in the shared library region, reached through `IExec` (chapter 8). Its main interface has the kernel calls but the ports' and connections', and lib/exec's functions but the messages', by the same names.

The tree's programs still call the functions directly; they move onto `IExec` as V6's later steps touch them. Code written either way compiles: the interface adds `IExec->`, it doesn't rename anything.
