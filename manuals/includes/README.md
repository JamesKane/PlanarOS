# Includes

*Planar Reference Manuals, volume 4.* The public headers, `include/planar/`. They are the system's C ABI ([docs §7.2](../../docs/07-language-toolchain.md)): any other language binds to Planar through them.

*Checked against V3.*

| Header | Contents | Used by |
|---|---|---|
| `planar/exec.h` | `Handle`; task, signal, Area, process and time calls; `AREAF_*`, `AREAR_*`; `struct AreaInfo`; `PROCF_*`, `PROCS_*`, `PFAULT_*`; `struct ProcessInfo`; `SIGBREAKF_CTRL_C` | Every program |
| `planar/ipc.h` | Ports and connections: the kernel calls, `struct ConnArena`, `struct ConnRing`, `struct Message`, `struct MinNode`, `CONN_*`, `CONNF_*` | `lib/exec`; servers that manage rings themselves |
| `planar/lease.h` | Device leases: the lease, interrupt, DMA Area and firmware calls; `LEASE_IRQS`, `LEASES_*`; `struct LeaseInfo`, `struct FirmwareInfo` | Drivers, `hwdb` |
| `planar/msg.h` | `struct MsgPort`, `MSGPORT_STASH` and Exec's message calls | Every program that sends or serves messages |
| `planar/syscalls.h` | Kernel call numbers (`SYS_*`) and errors (`ERR_NOSYS` to `ERR_BUSY`) | Stubs; any program that tests an error |
| `planar/endian.h` | `get_be32`, `get_le64` and friends, for firmware formats | Drivers, `hwdb` |

## What the headers promise

- **Layouts are fixed by `static_assert`.** `struct ConnArena` is one 64-byte cache line and `struct ConnRing`'s header two. A layout change bumps `CONN_VERSION`, and `AcceptPort` and `ConnectPort` refuse an arena of another version.
- **Call numbers and error values are meant to stay fixed** once a release has them, with new calls taking new numbers. **Proposed**: there has been no release yet, and nothing in `docs/` decides this.
- **A call that makes a handle returns `int64_t`**: the handle, or a negative error. `FindTask` is the one exception still declared `Handle`.
- **The per-process limits aren't in a header.** 256 handles, 64 tasks and 32 connection ends are kernel constants (`kernel/process.h`), and so is a port's queue of 32 (`kernel/port.c`). A program learns of them from `ERR_LIMIT` and `ERR_FULL`.
- **Fixed-width types only.** No `long`, no `ULONG`/`APTR` (VQ18). Pointers are 64-bit on every Planar platform.
- **C23.** The headers use `constexpr`, `nullptr`, `bool` and `[[noreturn]]`, and need a C23 compiler.

## Planned

One page per header, RKM style: each structure with its fields' meanings, and each constant with its value. They will be written when the headers settle after V2; until then the headers' own comments are the reference.
