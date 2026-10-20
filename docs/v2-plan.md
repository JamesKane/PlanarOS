# V2 plan: IPC

**Goal (§8):** connections, arenas, single-producer single-consumer rings with the `waiting` protocol, `DoMsg` handoff, death notices.

**Exit:**
- Zero-trap streaming is measured: kernel calls per message while the receiver stays busy.
- `DoMsg` costs no more than omega's 3,361 cycles per round trip on the RV2. **Met: 3,209 (identity), 3,283 (global).**
- omega's port-based A1 tests (`echo`, `client`, `pingpong`) pass, rewritten for connections, on every CPU, in both layouts, on QEMU and the RV2.

**Status: done (2026-10-20).** Steps 1–4, then hardening (V2.5) and the performance work below. Fewer lock pairs is parked for later (last section).

Each step ends with `make test-all` green and a `V2.n done` line in the transcript.

| Step | Content |
|---|---|
| **1. Ports and connections** ✅ | Named ports, connections with their arenas, accepting, closing, `SignalConn`. A fix for waking a task while holding a lock. Kernel-side tests. |
| **2. Rings** ✅ | The arena format and a user library (`lib/exec/msg.c`): `PutMsg`, `GetMsg`, `ReplyMsg`, `WaitPort` over the rings with the `waiting` protocol. `echo` and `client`. Streaming measured in kernel calls per message. |
| **3. `DoMsg`** ✅ | Send and wait for this message's reply, with the CPU handed straight to the server and priority lent to it. `pingpong` measures the round trip. |
| **4. Death and load** ✅ | Death notices tested from both ends: a client mid-`DoMsg` when its server dies, a server whose client dies. omega's "under load" run: a conversation kept up while other programs compete and crash. Soak, then the RV2. |

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

**V2.5: hardening (2026-10-20).** An adversarial review (docs/10-code-review.md) found 1 critical, 7 high and 17 medium problems. All are now fixed, or (M7) the docs changed to match the code. Each fix has a test that fails without it, except M1 and M9. The tests that can't pass by skipping (M16, M17) went in first. New conformance programs: `arearace`, `kexec`, `protrace`, `protflip`, `stashserver`/`stasher`, `vecfp`, `ctrwrite`, `limits`, `logflood`, plus a fourth rogue case. The kernel tests the device-tree parser against broken blobs, a part-way Area failure by fault injection, stranding and lowering in the scheduler, and handoff ahead of a ready peer.

The hunt for the soak failures that came before the review found three SMP races: a cross-CPU handoff deadlock, and two `finish_switch` races, one reading state and one reading the task's own page after letting go of it. 4,000 QEMU boots were clean afterwards.

Correct, then fast. On the RV2 the hardening first cost 252 cycles per kernel call, mostly from reading the current task with interrupts off. A single load through `tp` gets that back.

**Performance (2026-10-20): the V2 exit is met.** `make PROFILE=1` builds a cycle profiler (`kernel/prof.c`): marks along the kernel-call and switch paths sum the cycles per edge. On the RV2 it showed where the round trip went. What paid off:

| Change | `DoMsg` on the RV2, identity layout |
|---|---|
| Before | 3,512 cycles |
| Kernel calls save only what the calling convention keeps; scratch registers zeroed on the way out | 3,447 |
| A task that blocks goes into the scheduler with its run queue still locked | 3,368 |
| Relaxed ordering on hot-path hints and on reads under locks (the default puts a fence on each side of a load on RISC-V); handoffs counted per CPU | **3,209** |

That is below omega's 3,361 (3,283 in the global layout). The kernel-only Signal/Wait round trip is 1,554 cycles, and a null kernel call 469. A domain switch alone costs 118 cycles: 16-bit ASIDs, no flush. Lock pairs, at about 55 cycles each, are now the largest software cost. A round trip takes about a dozen of them.

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
- The peer works at least at the caller's priority until it next signals or waits. This is handoff boosting, not full priority inheritance; docs/04-ipc.md §4.4 lists what it doesn't cover.

`DoMsg` is `PutMsg`, then `SignalWaitConn` if the server is asleep, then collecting this message's reply. Any other reply that comes back first is set aside for `GetMsg`.

## Later: fewer lock pairs

**Parked (2026-10-20).** The V2 exit is met without this. It's written down so it isn't rediscovered from scratch.

**Where the pairs are.** A lock pair costs about 55 cycles on the X60: the interrupt mask, an `amoswap.aq` and a releasing store. One `SignalWaitConn` takes five or six:

| # | Lock | Taken in | Why |
|---|---|---|---|
| 1 | `port.c`'s one global `lock` | `conn_signal_wait_call` | the end, its peer and whether the peer is open |
| 2 | the peer's `t->lock` | `task_post` | its signals and its waiting mask |
| 3 | the peer's last CPU's `rq_lock` | `task_post` | its state. If it's handed to this CPU, that lock is dropped and this CPU's taken: one more pair |
| 4 | this task's `self->lock` | `end_loan` | only for a task holding a loan (the server's side) |
| 5 | `self->lock` again | `task_wait_kick` | did a signal come in meanwhile? |
| 6 | this CPU's `rq_lock` | `task_wait_kick`, let go in `schedule_locked` | mark waiting, pick the next task |

A round trip is two of these calls: the client's `DoMsg` and the server's reply-and-wait. That makes about a dozen pairs, or 600–700 of the 3,209 cycles.

**Ideas, cheapest first:**

1. **Fold `end_loan` into the wait.** `task_wait_kick` takes `self->lock` anyway; give the loan back under that hold, not before it. Saves pair 4 on the server's side. Small and local.
2. **A waiting task's state belongs to its `t->lock`.** A waiting task is on no run queue, so its last CPU's `rq_lock` has nothing to guard until the task is queued. If `WAITING → READY` moves under `t->lock`, a handoff takes only this CPU's `rq_lock`. Saves the extra pair in the cross-CPU case, and makes idea 3 simpler. Every reader of `t->state` has to be audited for the new rule.
3. **Post and wait as one operation.** A `task_post_wait(peer, sig, mask)` takes `self->lock` and the peer's `t->lock` in address order, then this CPU's `rq_lock` once. It queues the peer at the front, marks the caller waiting and goes into `schedule_locked` with that lock still held. Pairs 2, 3, 5 and 6 become three locks held together. This adds a lock-order rule: two task locks at once, lower address first. A peer that can't be handed over (wrong affinity, still switching away elsewhere) takes today's path.
4. **No global IPC lock on the fast path.** Pair 1 serialises every `SignalConn` on every CPU. That's a scaling problem before it's a cycle problem, and it will show once several servers run at once. Options:
   - a lock per connection: fixes the scaling, saves no cycles;
   - read the end without a lock, the way `handle_get` already reads the handle table, and free connections only after every CPU has passed a switch (a quiescent point), so a stale read never touches freed memory.
5. **No `irqsave` where interrupts are known to be off.** Several of these locks are taken inside another that already masked interrupts, so the CSR write and restore are wasted. Small, but it's on every pair.

**Estimate:** ideas 1–3 save about 4–6 pairs per round trip, roughly 250–350 cycles, which would put `DoMsg` near 2,900. Idea 4 matters more for many CPUs talking at once than for one round trip. These are estimates, not measurements.

**Before any of it lands**, because each idea changes the scheduler's locking:
- the profiler confirms the saving on the RV2;
- the hunt runs again: thousands of soak boots on both ISAs, and hang hunts that dump registers through the QEMU monitor, as for the three SMP races in V2.5;
- a mutation test for each new rule (a reversed lock order, a state change outside its lock);
- the lock order (task → run queue → timer, today only in code comments) is written into docs/02-architecture.md.

**When:** after V3, so the saving can be measured on an Arm core as well, or sooner if a test with several servers shows the global IPC lock contended.
