# 12. Fault tolerance: let it crash

**Status: proposed (2026-10-20).** The kernel and IPC pieces in §12.2 are built (V1, V2). The supervision and restart rules are proposed for V4 alongside the [driver model](11-driver-model.md).

Erlang's "let it crash" rule says that a process should not write defensive code to recover from its own bugs or corrupt state. When an invariant breaks, the process dies at once, the runtime reclaims everything it owned and tells its peers, and a supervisor starts a fresh copy in a known state.

Planar is a single-address-space OS in C23 driving real hardware, not a bytecode VM. Even so, its process model, handle table and connection rings give it the same runtime primitives. This document sets out how far Planar can take that rule, what has to change so restarts are clean, and where hardware forces a different answer.

## 12.1 What Erlang assumes

The BEAM virtual machine makes five promises that let code crash safely:

1. **Isolation.** A crashing process cannot scribble on another process's heap.
2. **Complete reclamation.** When a process exits, the runtime frees its memory, closes its ports and drops its timers without the process's help.
3. **Links and monitors.** Any peer waiting on the dead process gets an immediate `EXIT` or `DOWN` notice instead of hanging.
4. **Supervisors.** A parent process watches its workers and restarts them (`one_for_one`, `rest_for_one`, `one_for_all`) with bounded backoff.
5. **Fail-fast code.** Because steps 1–4 hold, trying to limp along after an unexpected error is worse than dying.

## 12.2 What Planar already has

Most of §§1–3 above already exist in `kernel/` and `lib/exec/`, or are proposed in [§11](11-driver-model.md).

| Erlang primitive | Planar mechanism | Status |
|---|---|---|
| Process isolation | One hardware protection domain per `struct process` (`kernel/process.c`). A user fault enters `process_fault`, logs the address and PC, and kills only that process. | Built (V1: `wild`, `rofault`, `wx`, `kexec`) |
| Complete reclamation | `reclaim` runs once every task of the process has switched off-CPU: `handle_free_owner`, `area_process_died`, `arch_domain_destroy` (P6, §2.3). | Built (V1, hardened in V2.5) |
| Process monitor (`DOWN`) | `process_create` records a `watcher` task and `watch_sig`, signalled in `reclaim` when the process is gone. | Built (V1) |
| Bilateral link (`EXIT`) | When either end of a connection closes or dies, `close_end` sets `CONNF_CLIENT_GONE` or `CONNF_SERVER_GONE` in `ca_Closed` and signals the peer (`kernel/port.c`). `DoMsg` wakes with `ERR_PORT_DIED`. | Built (V2: `doomed`, `talker`, `waiter`) |
| Blast-radius containment | Each connection has its own shared `ConnArena`. `lib/exec/msg.c` checks ring indices and message bounds before touching them; a corrupt arena breaks only that connection (`mp_Broken`). | Built (V2.5: `rogue`, `stashserver`) |
| Crash-only teardown | **D1 (Unbind is kill):** graceful stop is an optional `STOP` message before killing the process, so there is only one teardown path (§11.4). | Proposed (V4) |
| Hardware stop on crash | **D2:** the kernel masks IRQs at exit, quarantines lease-tied DMA Areas, and holds them until the lease's stop method has reset or gated the device (§11.4). | Proposed (V4) |
| No state spill into servers | Provider servers (`power`, GPIO, clocks) keep client votes per connection and drop them when `ca_Closed` is set (§11.5). | Proposed (V4) |

## 12.3 Principles

**Proposed.** These extend P3 and P6 (§0.2) and D1–D8 (§11.3).

- **F1. The process is the unit of failure.** Tasks inside a process share one protection domain and one heap. If one task hits an invariant failure or a hardware fault, the whole process exits (§12.4).
- **F2. Fail fast on internal bugs; validate at boundaries.** A server or driver that sees a corrupt internal structure calls `ExitProcess`. A server that sees bad input on a connection or from a device rejects the input or drops that connection, and stays up (§12.4).
- **F3. No cross-process locks in shared memory.** Processes coordinate through connections, never by taking mutexes in a shared Area. A dead peer may never leave another process waiting on a lock word (§12.5).
- **F4. Names survive restarts.** A supervised service keeps its port name across a crash, so clients see a dropped connection and a fresh one, not a missing service (§12.6).
- **F5. Every long-lived process has a supervisor.** The supervision tree is explicit and acyclic, following the dependency graph (§12.7).
- **F6. Never crash what cannot be reset.** The kernel, unconfined DMA without a stop method, and firmware that wedges the SoC on abrupt teardown are outside the crash boundary (§12.8).

## 12.4 The process is the unit of failure

### Tasks vs. processes (F1)

In Erlang every actor has its own heap. In Planar a process is a protection domain, and it may run several tasks (`CreateTask`) that share memory.

`process_fault` already kills every task in the process (`kill_tasks`), because sibling tasks share the address space that just faulted. User code must follow the same rule:

- `ExitTask` is for normal completion of a worker thread, never for error recovery.
- An assertion failure, an out-of-range internal state, or an unexpected kernel error in any task calls `ExitProcess`, taking the whole domain down so its supervisor can restart it cleanly.

### Internal invariants vs. external input (F2)

"Let it crash" does not mean crashing when a client sends garbage. In a single-user OS one server serves many clients, and a bad client must not take the server down (`rogue`).

The boundary is ownership:

| Fault source | Action | Why |
|---|---|---|
| Corrupt state in the process's own memory | `ExitProcess` | The process has a bug; continuing risks worse damage. |
| Unexpected error from a supplier that cannot be retried | `ExitProcess` | Let the supervisor restart in dependency order. |
| Bad ring indices or bad message pointer in `ConnArena` | Mark `mp_Broken`, `ClosePort` that connection | The client broke its own arena; other clients are unaffected (`lib/exec/msg.c`). |
| Invalid arguments in a client's request | Reply with `ERR_BADARG` | Normal error return after copying arguments out of the arena (§4.3). |
| Out-of-range descriptor or status word from hardware | Log, ask `hwdb` for a device reset (§11.10), or `ExitProcess` | Device data is untrusted input (§11.8); killing the driver triggers the §11.4 stop method and a clean rebind. |

## 12.5 Shared memory without state spill

### Why shared Areas complicate "let it crash"

Erlang's isolation comes from copying messages. Planar shares memory (`ShareArea`, `IOF_LEND`) so bulk payloads never copy (P2, P3). If two processes mutate a shared Area together and one crashes mid-write, the survivor is left holding torn state.

Worse, §2.2 and §4.1 propose `WaitAddr` and `WakeAddr` (futexes) on words in shared memory. If Process A takes a futex-backed lock in a shared Area and crashes, the kernel reclaims A's mapping, but Process B is left sleeping on `WaitAddr` forever, or wakes to find a locked mutex with no owner.

### Rules for shared memory (F3)

**Proposed:**

1. **No cross-process mutexes.** `WaitAddr` and `WakeAddr` are for tasks *within* the same process, or for single-writer monotonic sequence numbers (such as the 64-bit sync objects in §5.1). When a process dies, step 6 of §2.3 wakes any `WaitAddr` waiters on pages tied to its leases or connections so they can check `PortGone` or the lease fault signal.
2. **Control stays on connections.** Requests, replies and ownership handoffs go through connection rings, where the kernel sets `ca_Closed` and wakes the peer on death.
3. **Bulk Areas are single-writer at a time.** When a client lends a buffer Area to a server or driver (`IOF_LEND`), the client does not touch it until the reply arrives (§4.3). If the server crashes during the request, the client gets `ERR_PORT_DIED`, knows the buffer's contents are indeterminate, and either retries the read/write or discards the buffer.
4. **No pointers or locks in DMA or lent pages.** Shared payload Areas carry raw data or hardware descriptors, never C pointers or library state (§11.8).

## 12.6 Ports across a restart

### The gap in V2

Today a port belongs to the task that called `CreatePort` (`kernel/port.c`). When that server crashes:

1. `ipc_task_gone` calls `drop_port`.
2. `drop_port` removes the port's name from the kernel's `ports` list and fails any queued, unaccepted connections with `CONNF_SERVER_GONE`.
3. Existing accepted connections get `CONNF_SERVER_GONE` and wake with `ERR_PORT_DIED`.

Between step 2 and the moment the restarted server calls `CreatePort` again, the name does not exist. Any client that tries to reconnect immediately after seeing `ERR_PORT_DIED`—or any new client calling `OpenPort` during the restart window—gets `ERR_NOTFOUND`. Conformance programs currently spin in a `while (ConnectPort(...) == ERR_NOTFOUND)` loop (`conformance/doomed.c`, `conformance/client.c`).

### Proposed fix: supervisor-held ports (F4, VQ28)

For a supervised server or driver, the port name should not vanish while the process restarts:

- **Option A: Port handoff from the supervisor.** The supervisor (`init` or `hwdb`) creates the named port before spawning the server and passes the port handle (or transfers ownership) to the child. When the child dies, its accepted connections still close with `CONNF_SERVER_GONE`, so clients learn of the crash at once. The port itself stays in the name table, and new `OpenPort` calls queue on it while the replacement process starts and calls `AcceptConn`. If the supervisor gives up after the restart cap (§11.10), the supervisor deletes the port, which drains the queue with `CONNF_SERVER_GONE`.
- **Option B: Kernel wait-for-port.** Keep `CreatePort` in the server, and add a timeout or wait flag to `OpenPort` so a client blocks until the name is published (or the wait times out) instead of spinning on `ERR_NOTFOUND`.

Option A matches how `inetd`, `launchd` and systemd socket activation make restarts invisible to connecting clients, while still delivering `ERR_PORT_DIED` on connections that were open when the old instance died.

## 12.7 The supervision tree

Erlang systems work because every process sits in a tree with a stated restart policy. Planar needs the same structure in user space (**VQ29**).

```
                        ┌──────────────────────┐
                        │  kernel → init       │
                        └──────────┬───────────┘
         ┌─────────────────────────┼─────────────────────────┐
         ▼                         ▼                         ▼
  ┌──────────────┐          ┌──────────────┐          ┌──────────────┐
  │  hwdb        │          │  dos         │          │  display     │
  │ (device DAG) │          │ (namespace)  │          │  input / net │
  └──────┬───────┘          └──────┬───────┘          └──────┬───────┘
         │                         │                         │
   ┌─────┴─────┬─────────┐         ▼                         ▼
   ▼           ▼         ▼   ┌───────────┐             ┌───────────┐
┌──────┐   ┌──────┐  ┌─────┐ │ fs        │             │ apps      │
│power │   │ pci  │  │leaf │ │ handlers  │             └───────────┘
│acpid │   │ bus  │  │drvs │ └───────────┘
└──────┘   └──────┘  └─────┘
```

### Leaf drivers and stateless servers (`one_for_one`)

- Managed by `hwdb` (§11.6, §11.10).
- When a leaf driver (`uart`, `sd`, `nvme`, `gpu-*`) crashes:
  1. The kernel runs the exit path: masks IRQs, closes connections (`ERR_PORT_DIED`), quarantines DMA Areas, signals `hwdb`.
  2. `hwdb` runs the lease's stop method (§11.4) and releases the quarantined Areas.
  3. `hwdb` spawns a fresh driver process for the node, with exponential backoff and a retry cap.
  4. Class libraries (`blk`, `serial`) reconnect and retry idempotent requests (§11.9).

### Bus drivers and providers (`rest_for_one` along the graph)

- The `hwdb` dependency graph (§11.5) already records which consumers depend on a bus (`pci`) or provider (`power`, `acpid`, a GPIO controller).
- If a bus driver crashes, its child nodes' drivers cannot keep running against a dead bus. `hwdb` kills the subtree in reverse graph order (§11.4), stops the bus hardware, and restarts forward along the graph.
- What if `power` crashes? Every driver's power votes live on its connection to `power` (§11.5). Rather than resetting the whole board when `power` restarts, `power` can reconstruct active votes in one of two ways (**VQ30**):
  - **Re-query from `hwdb`:** since `hwdb` powers a node on before granting its lease (D3, §11.5), `hwdb` already knows the set of powered nodes and re-sends that state to `power` when `power` reconnects.
  - **Rest-for-one fallback:** if `power` cannot reconcile hardware state without glitching clocks, `hwdb` treats a `power` crash as a subtree reset for the affected domains.

### Filesystems (`dos` and handlers)

- `dos` holds the namespace and assigns; each mounted filesystem (`fat32`, native FS) runs as a handler process in its own domain.
- A crash in a filesystem handler drops its clients' open file connections with `ERR_PORT_DIED`.
- Under **VQ14** (journaled or CoW metadata for the native filesystem), the on-disk structure is always consistent after a handler crash. `dos` restarts the handler, which replays its journal or mounts the last committed root, and applications reopen their files.

### What if `hwdb` itself crashes?

- `hwdb` holds the device graph. Its inputs—the FDT or static ACPI tables, the board quirk tables, and the `.note.planar.match` sections—are read-only in the boot image (§9.4, §11.6).
- If `hwdb` is kept stateless with respect to live kernel leases (because the kernel's handle table owns the active leases and processes, §11.2), a restarted `hwdb` could rebuild the static graph from the read-only boot Area and query the kernel for live leases.
- For V4, however, `hwdb` and `init` can be treated like Erlang's kernel application: if `hwdb` crashes after exhausting its own assertions, the system logs a meditation and reboots cleanly via the watchdog (`plat/ky-x1/wdt.c`).

## 12.8 Where "let it crash" stops

Erlang's rule relies on the VM staying intact beneath the crashing actor. Planar has three places where a crash cannot be made cheap or invisible (F6):

1. **The kernel (`kernel/`, `arch/`, `plat/`).** The kernel is Planar's BEAM VM. It stays small (10–15k lines, §2.2), never uses dynamic code, and checks every user pointer and handle. A kernel bug is a fatal meditation.
2. **Unconfined DMA with no stop method (VQ9, VQ24).** On boards without a usable IOMMU in front of a peripheral (K1 DPU, Q8B hypervisor-policed SMMU), isolation protects against bad CPU pointers in the driver, not against a driver that programs a wild DMA address into the device before crashing. And if the device has no reset line, power domain or bus-master bit, every crash leaks its quarantined DMA Areas until reboot (§11.4). Those drivers must be written and reviewed with kernel-level care.
3. **Hardware that wedges the SoC on abrupt stop (VQ16).** Qualcomm's Hexagon CDSP hangs the Q8B if PAS-shut-down while running (§5.3), and reading an unpowered CIX GPU register raises SError (§5.3). D3 (power before lease) and lease stop methods keep user crashes away from those traps, so the teardown sequence itself never violates hardware rules.

## 12.9 Effects on other documents

| Document | Change |
|---|---|
| [02-architecture.md](02-architecture.md) §2.3 | Cross-reference F1 (the process is the unit of failure) and §12.5 on waking `WaitAddr` waiters at exit. |
| [04-ipc.md](04-ipc.md) §4.1 | Note F3: `WaitAddr`/`WakeAddr` is for intra-process locks or monotonic sequence numbers, never cross-process mutexes. |
| [11-driver-model.md](11-driver-model.md) §11.10 | Link §12.6 (ports across a restart) and §12.7 (the supervision tree). |
| [questions.md](questions.md) | Add VQ28 (ports across a restart), VQ29 (user-space supervision tree), and VQ30 (`power` state recovery). |
