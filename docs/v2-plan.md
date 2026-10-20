# V2 plan: IPC

**Goal (§8):** connections, arenas, single-producer single-consumer rings with the `waiting` protocol, `DoMsg` handoff, death notices.

**Exit:**
- Zero-trap streaming is measured: kernel calls per message while the receiver stays busy.
- `DoMsg` costs no more than omega's 3,361 cycles per round trip on the RV2.
- omega's port-based A1 tests (`echo`, `client`, `pingpong`) pass, rewritten for connections, on every CPU, in both layouts, on QEMU and the RV2.

Each step ends with `make test-all` green and a `V2.n done` line in the transcript.

| Step | Content |
|---|---|
| **1. Ports and connections** ✅ | Named ports, connections with their arenas, accepting, closing, `SignalConn`. A fix for waking a task while holding a lock. Kernel-side tests. |
| **2. Rings** ✅ | The arena format and a user library (`lib/exec/msg.c`): `PutMsg`, `GetMsg`, `ReplyMsg`, `WaitPort` over the rings with the `waiting` protocol. `echo` and `client`. Streaming measured in kernel calls per message. |
| **3. `DoMsg`** ✅ | Send and wait for this message's reply, with the CPU handed straight to the server and priority lent to it. `pingpong` measures the round trip. |
| **4. Death and load** | Death notices tested from both ends: a client mid-`DoMsg` when its server dies, a server whose client dies. omega's "under load" run: a conversation kept up while other programs compete and crash. Soak, then the RV2. |

Step 1 delivers `kernel/port.c` and `conformance/conn.c`. Two processes connect, check that they share the arena and trade signals. The server leaves a port behind with an unaccepted connection, the client leaves a connection open, and the kernel closes both when each exits. Mutation tests confirm the checks catch: a switch inside a lock, no self-IPI, no close flag, and no cleanup when a task ends. The close *signal* can't be checked by timing here, because the server usually closes before the client blocks. Step 4 kills a server whose client is known to be blocked.

Step 2 delivers:
- the arena format (`include/planar/ipc.h`) and `lib/exec/msg.c` (`include/planar/msg.h`), linked into every user program;
- `echo`, a server for up to 8 clients that sleeps only when every connection is empty;
- `client`, which runs round trips and streaming.

Streaming 20,000 messages with the ring kept full took about 24–32 kernel calls per 1,000 messages on each side, on QEMU with 4 CPUs. That is a call only each time one side ran dry. The test fails at one call per 4 messages.

Mutation tests: a producer that never signals hangs the round trips, and one that always signals fails the streaming bound. A consumer that doesn't look again after setting `cr_Waiting` passes a single boot but fails 26 of 40 boots in `make soak`, so that check lives in the soak. The real code passed 250 boots: 100 RV64, 100 AArch64, and 50 RV64 in the global layout.

Step 3 delivers `DoMsg` over `SignalWaitConn`, and `pingpong`. Kernel tasks test the mechanism directly:
- A server waiting on another CPU runs on the waker's.
- Lent priority 10, it runs past a priority-5 task hogging the CPU.
- It runs ahead of a ready peer at priority 10.
- It is back at priority 1 once it waits again.

On QEMU, about 98% of `DoMsg` round trips hand off. Each round trip costs the client one kernel call and the server two (`SignalConn` for the reply, then `Wait`). Mutation tests confirm each of these checks catches: no handoff, no loan, a loan that outlives the next wait, and a handoff queued at the back. Waiting ends a loan without an extra switch, unless the wait returns at once. 250 more soak boots passed.

Step 1 also serialises `kprintf` across CPUs. Lines from different CPUs used to interleave mid-line.

## Design

This follows docs/04-ipc.md. Where V2 settles something that doc leaves open, the doc is updated too.

**Ports.** `CreatePort(name, sigbit)` makes a named rendezvous, owned by the calling task. A port has no ring. When a client connects, the kernel queues the connection on the port and signals the server with `sigbit`. `DeletePort` removes the name and refuses connections not yet accepted.

**Connections.** `OpenPort(name, sigbit, slots, heap, &arena)` makes a connection:
- The kernel allocates the arena Area and maps it read-write into the client.
- It writes the arena header and two empty rings of `slots` entries (VQ10: ring size per connection).
- The remaining `heap` bytes are for messages.
- It queues the connection on the port.

The server takes it with `AcceptConn(port, sigbit, &arena)`, which maps the same Area into the server at the same address. Each side holds its own handle to the connection, and names the signal bit it wants to be woken with.

**The only trap on the fast path** is `SignalConn(conn)`. It signals the other end's task with the bit that end chose, and sends an IPI if that task is on another CPU. Rings and messages live in user memory; the kernel never reads them.

**Endpoints belong to tasks.** Signals go to tasks, so an endpoint belongs to the task that opened or accepted it. If that task ends, the endpoint closes. The same goes for a port and the task that created it.

**Death notices.** When one end closes, the kernel:
- sets that end's bit in the arena header's `ca_Closed`;
- signals the other end;
- unmaps the arena from the closing process.

This happens whether the end closed by `CloseConn`, by its task ending, or by its process dying. The arena goes when neither end maps it. The library sees `ca_Closed` and fails what is outstanding with `ERR_PORT_DIED`. The kernel doesn't walk the rings to do it.

**Trust.** Either side can scribble on the arena, but only on its own connection.
- The library checks every message pointer it takes from a ring: aligned, with its header inside the arena.
- It checks ring indices against the ring size, and treats a broken ring as a closed connection.
- Servers still copy what they validate (the omega rule).
- V2 messages live in the arena. `IOF_LEND`, which lends other Areas, comes later.

**Replies.** A reply goes back on the connection the request came in on, so `struct Message` needs no reply port.

**Waking with a lock held.** `task_signal` used to switch straight to the woken task when it outranked the caller on the same CPU, even inside a spinlock. `sys_signal` calls it from inside the handle table's lock, so a user `Signal` to a higher-priority task on the same CPU switched away with that lock held. Nothing in V1 had two user priorities, so nothing showed it. Now:
- A wake inside a lock, with interrupts held off, only marks the CPU.
- The switch comes when the lock is let go: an IPI to itself, taken once interrupts are back on.
- IPC code wakes after dropping its own lock wherever it can, to avoid that self-IPI.

**Handoff.** `SignalWaitConn(conn, mask)` signals the other end and waits.
- If that wakes the peer, and the peer may run on this CPU, the kernel queues it at the front of this CPU's queue, so the wait switches straight to it.
- The peer works at least at the caller's priority until it next signals or waits (priority inheritance).

`DoMsg` is `PutMsg`, then `SignalWaitConn` if the server is asleep, then collecting this message's reply. Any other reply that comes back first is set aside for `GetMsg`.
