# 9. Queue leases: GPU work from user space

*Checked against V5.*

A coprocessor runs work from a queue. On most systems the kernel owns that queue, and every job is a system call. In Planar the app owns it: it writes commands into a ring in its own memory, tells the device, and learns that a job is done from a word the device writes. Nothing passes through the kernel per job, and where the hardware allows it, nothing passes through a server either. This is a **queue lease** ([docs §5.1](../../docs/05-coprocessors.md)). V5 builds it for the GPUs on both boards. The class is the [GPU class](../devices/09-gpu.md), which has the messages, the structures and the commands; this chapter is what any queue lease is made of, and how a program lives with one.

## Concepts

### The parts

| Part | Where it lives | Who writes it |
|---|---|---|
| **The ring** | The app's own memory, in an Area it lends the server | The app |
| **The insert count** (`gq_Insert`) | A page the server maps to the app, read-write | The app |
| **The extract count** (`gq_Extract`) | A page the server maps to the app, read-only | The device's firmware |
| **The doorbell** | A page of the device's registers mapped to the app, or a message to the server | The app |
| **The sync object** (`struct GpuSync`) | The app's memory, lent with the ring | The device |
| **Faults** | The server, asked with a message | The server |

The counts only go up: `gq_Insert` is every byte ever written, `gq_Extract` every byte read. The ring's offset is the count modulo its size.

### Two providers, one API

- **A hardware queue.** The device's firmware reads the app's ring itself, and the app rings a doorbell page of its own. The server isn't in the job's path at all. The CIX's Mali-G720 works this way (`gpu-panthor`): each queue is a CSF group pinned to a slot, with the slot's doorbell shared to the app by `ShareLease`.
- **A server queue.** The device can only be told about new work through a privileged register. The app writes its ring the same way, then sends the server one message, and the server kicks the device. The RV2's BXE-2-32 works this way (`gpu-rogue`): the kick is a kernel CCB command and a write to `MTS_SCHEDULE`.

`gm_Doorbell` from `GPU_QUEUE` says which: a page to write 1 to, or 0, meaning send `GPU_KICK`. Everything else is the same, so code that rings or kicks behind one function runs on either.

### What the kernel provides

A queue lease needs no kernel call of its own. It's built from these:
- **`AREAF_NC` Areas** for the ring and the sync object. Neither GPU sees what's still in the CPU's caches.
- **`ShareArea` and `ReleaseArea`**: the app lends an Area to the server, and the server lets go of it when the queue ends.
- **`ShareLease`**: one page of the server's registers, the doorbell, mapped into the app.
- **`WaitAddr` and `WakeAddr`** (chapter 7): the app sleeps on its sync object, and the server wakes it when the firmware says it moved.
- **`LimitLeaseDma`**: hwdb's limit on where the device can reach. The RV2's GPU sees only the first 2 GiB at their own addresses.

### Lifetimes

- **The queue lasts as long as the app's connection.** When the connection goes, for whatever reason, the server stops the queue first (the group terminated, or the context cleaned up by the firmware), then takes the app's memory out of the GPU's view, then releases what the app lent. Nothing reads the app's memory once it's gone.
- **When the server dies**, hwdb stops the device (a soft reset on both GPUs) before the server's memory, and what apps lent it, leaves quarantine. The doorbell pages go with the server's death. The app sees its connection go (`PortGone`), and its next job needs a new connection and a new queue.
- **A fault** stops the app's queue alone, and the server keeps the last one for `GPU_STATUS`. The app's sync object doesn't move. In V5 the app finds out by asking or by a deadline; a message on the connection is later.

## Using it

The setup is four messages: `GPU_OPEN`, `GPU_MAP` for each Area lent, then `GPU_QUEUE`. [Devices ch. 9](../devices/09-gpu.md#using-it) has it in full, with a job for each GPU. A job is always the same four moves:
```c
/* 1. The commands, at the ring's insert offset. */
write_commands(&ring[in->gq_Insert % RING]);
drv_io_barrier();
/* 2. The insert count, past them. */
in->gq_Insert += bytes;
drv_io_barrier();
/* 3. The doorbell, or the server. */
if (doorbell)
    *doorbell = 1;
else
    ask(GPU_KICK);
/* 4. The sync object: spun on, or slept on. */
while (sync->gs_Seqno != want)
    WaitAddr(&sync->gs_Seqno, want - 1, deadline);
```

### Living through the server's death

From `conformance/gputest.c`'s load mode, which runs jobs without a pause while hwdb kills servers around it, and the GPU server last:
```c
while ((now = CurrentTime()) < end) {
    /* A dead server's doorbell faults: none rung once it's gone. */
    if (!PortGone(&mp)) {
        submit();
        if (spin(now + 1000 * MS))
            continue;
    }
    /* A second and no update: only the server's death may do that. */
    CHECK(PortGone(&mp));
    ClosePort(&mp);
    CHECK(take() == 0);                 /* connect and GPU_OPEN, retried */
    CHECK(queue() == 0);                /* lend new Areas, GPU_QUEUE */
}
```
A new server knows nothing of the old queue: the app lends its memory again and starts its counts from 0. The look at `PortGone` before the doorbell narrows the window for a fault, but doesn't close it: a server that dies between the look and the write takes the app with it. Nothing catches that fault yet.

## Rules

- **Lend whole `AREAF_NC` Areas**, shared read-write with the server, and keep them until the queue ends. `GPU_UNMAP` is refused while there's a queue.
- **Order the writes:** the commands, then `gq_Insert`, then the doorbell or kick, with a barrier between each. The device reads them without the CPU's caches, and the doorbell is a device write.
- **Never get more than a ring ahead.** Keep `gq_Insert − gq_Extract` within the ring's size. The firmware moves `gq_Extract` when it gets round to it, not at each job, so leave room.
- **Wait with a deadline.** A queue that faulted never moves its sync object. After a deadline, ask `GPU_STATUS`.
- **Don't ring a dead server's doorbell.** Its page is gone, and the write faults.
- **One queue per connection.** A GPU has a few queues to give: 7 on each board. `gm_Free` from `GPU_OPEN` says how many are left.

## Costs

Measured by `gputest` on each board. "Machine-wide" counts every CPU's calls, the test's own timing included.

| | The CIX (hardware queue) | The RV2 (server queue) |
|---|---|---|
| First job, submit to update | 25–50 µs | 52 µs |
| Kernel calls meanwhile, machine-wide | 1, the test's own | 4–8 |
| A job, spun on, 1,000 in a row | 11 µs | 53–54 µs |
| Each of those: kernel calls, crossings, machine-wide | 6, 2: off the job's path | 7.1, 2.0: on it |
| A job slept on, woken by the server | 22–23 µs (worst 25 µs) | 55–56 µs (worst 72 µs) |
| Jobs in 30 s while other servers are killed | 2,315,511 | 510,000–527,000 |

On the CIX the server isn't in the job's path, but it still runs after each job: it takes the firmware's sync update, to wake the word for anyone who sleeps on it. An app that only spins pays for that on another CPU.

## Differences from Exec, and from Linux

- **Exec had no coprocessors** to share. Its nearest relative is an `IORequest` to a device, which was a message per request: that's the server queue, not the hardware one.
- **Linux's panthor rings every doorbell itself**, from a system call per job. Planar's hardware queue gives the app the doorbell page; that's the difference between the two columns above.

## Functions used

[AllocArea](../autodocs/exec.md#execallocarea), [ShareArea](../autodocs/exec.md#execsharearea), [ReleaseArea](../autodocs/exec.md#execreleasearea), [WaitAddr](../autodocs/exec.md#execwaitaddr), [WakeAddr](../autodocs/exec.md#execwakeaddr), [ShareLease](../autodocs/lease.md#leasesharelease), [LimitLeaseDma](../autodocs/lease.md#leaselimitleasedma), [PortGone](../autodocs/msg.md#execportgone); the class: [Devices ch. 9](../devices/09-gpu.md)
