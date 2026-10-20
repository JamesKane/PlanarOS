# 11. Driver model: lessons from other systems

**Status: proposed (2026-10-20).** Nothing here is built. Drivers arrive with V4 ([roadmap](08-roadmap.md)).

The evidence comes from the AbyssBSD driver-model survey and design, written in October 2026 (`~/Desktop/DRIVER-MODEL-SURVEY.md` and `DRIVER-MODEL.md`). They cover FreeBSD, NetBSD and OpenBSD, illumos, Linux and Rust for Linux, IOKit and DriverKit, WDM and WDF, Fuchsia, the Rust research kernels, seL4's sDDF, Genode, QNX and MINIX 3. References in the form *S§n* are to sections of the survey.

AbyssBSD is a monolithic Rust kernel, and its answer is shaped by that. Planar's drivers are C programs in their own protection domains (P3). So this document doesn't copy AbyssBSD's design. It asks, for each failure the survey found, whether Planar already avoids it, avoids it at a cost, or doesn't avoid it yet.

## 11.1 What the survey found

Every system that leaves these jobs to the driver gets the same bugs (S§13.2):

1. Asynchronous entry points that outlive teardown: callouts, interrupt handlers, open files. One of these hard-hung the Q8B under AbyssBSD.
2. Confusing the device's lifetime with the driver binding's and with the lifetime of objects users can see (Linux `devm`).
3. Ordering by convention: numeric passes, blind retries, registration order.
4. Races between cancelling and completing a request (WDM IRPs).
5. Doing the wrong thing in the wrong execution context (sleeping at raised IRQL).
6. DMA ownership errors, and kernel data in pages a device can reach (Thunderclap).
7. Touching a device whose power domain is off. That resets the Q8B and raises SError on the CIX GPU.
8. Holding a lock across probe, which deadlocks when probe calls back into the bus.

The mature systems converged on the same answers (S§13.3): matching is data, the framework owns the lifecycle, dependencies form an explicit graph, concurrency is declared, device classes own the protocol logic, and DMA goes through one abstraction with the IOMMU behind it.

On isolation, the evidence is about cost (S§13.4):

| Mechanism | Measured cost | Source |
|---|---|---|
| A thread hop or channel per driver call | +50–150% CPU, worst at queue depth 1–2 | Fuchsia RFC-0126 |
| An unbatched domain crossing per I/O | 29.5 → 5.3 Mpps | RedLeaf |
| A user-space driver with a serial queue | about 12% throughput | Apple forum report |
| A dynamic IOMMU map and unmap per I/O | up to 8.3% | Asterinas |
| Batched rings between isolated components | Link saturated on one core where Linux used the whole CPU at half the rate | seL4 sDDF |

Planar has already chosen user-mode drivers (P3), so this table isn't a reason to change that. It shows where that choice costs us, and that sDDF-style batched rings are what makes it affordable.

## 11.2 What Planar gets for free

Some of the survey's hardest problems are solved by the architecture we already have. They should be kept that way deliberately, not by accident.

| Survey problem | Why Planar avoids it | Keep it by |
|---|---|---|
| No general drain at detach (class 1) | A driver is a process. Everything that can reach into it (connections, timers, IRQ bindings, leases) is a handle the kernel reclaims at exit (P6, §2.3). | Never giving a driver a way to register anything outside a handle. Provider-side state must be per connection (§11.5). |
| A binding ledger (AbyssBSD §13.1) | The handle table *is* the ledger. | Making every lease and grant a handle. |
| Revocable handles for user-visible objects (Rust `Revocable`, AbyssBSD `Gate`) | Death notices already exist (V2). When a server dies, `ca_Closed` is set and outstanding requests fail with `ERR_PORT_DIED`. Open connections survive as objects and can be closed. | Having class libraries treat `ERR_PORT_DIED` as "the device went away", not as a crash (§11.9). |
| Serialised by default (IOKit work loop, WDF sync scope) | A driver with one task running `Wait(mask)` is serialised already. Exec code reads naturally this way. | Making extra tasks an explicit, per-queue opt-in (§11.7). |
| Execution-context bugs (class 5) | Driver code always runs in a user task. The kernel's generic handler takes the place of a filter. A driver can't run at interrupt level, so it can't sleep there. | Keeping user-mode fast interrupt handlers (§4.6, "later") out until a measurement asks for them. |
| A driver crash takes down the system | It takes down a process. | §11.4 makes sure the device stops as well. |

## 11.3 Principles

**Proposed.** Each comes from a failure in §11.1.

- **D1. Unbind is kill.** There is one teardown path, and it is the one that has to work when the driver has crashed. A graceful stop is an optional prelude to it, never a separate path (§11.4).
- **D2. The framework stops the hardware.** When a driver dies, something other than the driver puts its device into a state where it can't DMA or interrupt. Areas a device could still write are never returned to the allocator first (§11.4).
- **D3. No lease before power.** The hwdb grants a node's MMIO only after the `power` server reports the node on. Drivers don't sequence power themselves (§11.5).
- **D4. Order comes from the graph.** The `hw_ref`s the hwdb already records are the dependency graph. Start, shutdown and suspend follow it. A wait always names what it waits for (§11.5).
- **D5. Matching is data.** A driver's match keys and ranks are a table the hwdb reads without running driver code. Ambiguity is a build error (§11.6).
- **D6. Calls go down the graph; notices come up.** A consumer may `DoMsg` its supplier. A supplier never `DoMsg`s a consumer, and the hwdb never `DoMsg`s a driver. That rules out deadlock cycles across processes (§11.6).
- **D7. No extra crossing on the data path.** The number of domain crossings per I/O is a design parameter, counted and measured like `DoMsg` cycles (§11.8).
- **D8. Classes own protocols.** A block driver speaks the block protocol, not its own. Multiplexing, partitions, cancellation and naming live outside leaf drivers (§11.9).

## 11.4 Teardown: unbind is kill

### The gap

§2.3 reclaims a dead process in this order: stop the tasks, return unreplied messages, send death notices, revoke leases, free Areas. For coprocessor leases step 4 "drains or resets" the queue (§5.4). For ordinary device leases nothing says how the device is stopped. If a block or network driver dies with DMA in flight, step 5 frees Areas the device is still writing into. That is the same bug as the AbyssBSD warm-reset case (S§13.2 class 6), except that here the memory is reused at once.

### Design

**Proposed.**

1. **A lease knows how to stop its device.** When the hwdb grants a lease, it records a *stop method* taken from the node:

   | Device | Stop method | Run by |
   |---|---|---|
   | PCIe function | Clear Bus Master Enable, then FLR if the function supports it | The `pci` bus driver |
   | SoC block with a reset line (`resets`, or a quirk) | Assert the reset, then gate the clocks | `power` |
   | SoC block with a power domain only | Power the domain off | `power` |
   | Behind an IOMMU (P8 boards) | Detach the device's context, so further DMA faults | hwdb, through the kernel |
   | None of the above | No stop method: see step 3 | — |

2. **DMA Areas are tied to the lease.** A driver allocates DMA memory with `AllocDmaArea(lease, …)` (§11.7). The kernel marks such Areas with the lease. At exit, step 5 frees ordinary Areas as now. It moves lease-tied Areas to a **quarantine list** instead. They return to the allocator only when the hwdb reports that the stop method has run.
3. **No stop method means no reuse.** If a device has no stop method, or the stop doesn't complete within a bound, its quarantined Areas are leaked for the rest of the boot and the kernel logs it. Leaking memory is better than a device scribbling on whatever gets that memory next. `QuerySystem` reports the quarantined total.
4. **IRQ lines are masked by the kernel** at step 1, before anything else happens. The kernel owns `BindIrq`, so it doesn't need anyone's help.
5. **Graceful unbind is a prelude.** To unbind a working driver (device removal, a rebind, shutdown), the hwdb sends it a `STOP` message with a deadline. The driver library fails or finishes outstanding requests, quiesces the device and replies. Then the hwdb kills the process anyway, and the normal exit path runs. If the deadline passes, the hwdb kills it without waiting. Shutdown and reboot use the same path, in reverse graph order (§11.5).

This is crash-only design. The forced path has to be correct, because drivers crash. Making it the only path means every unbind tests it, and there's no second teardown path to get wrong (S§13.2 classes 1 and 2).

`STOP` in step 5 also gives AbyssBSD's warm-reset protection for free: at reboot or kexec, every device is stopped before firmware takes over.

## 11.5 Ordering and power

### The graph

The hwdb's `hw_ref`s (power domain, clock, reset, regulator, PHY, and from ACPI `_DEP`, `GpioInt`/`GpioIo`, `*SerialBus`, `_PR0`, and PCC) are already supplier → consumer edges. **Proposed:** the hwdb uses them as the only source of order, as Linux device links and AbyssBSD §6 do:

- A node is **ready** when every required supplier is bound. Only then is its driver started.
- When a node binds, the hwdb re-examines **only that node's consumers**. There is no global retry and no deferred-probe loop (S§5.4.2).
- A node that can't become ready is in a named state: waiting for a supplier node, waiting for a reference no node provides (the CIX `I2C0.UXC0` case), or in a cycle. `QuerySystem`, or a hwdb query, lists them with the chain of what's missing. That is a diagnosis, never a hang.
- An edge is **required** or **optional**. Cycles are broken by dropping optional edges first.
- **Shutdown and suspend** go in reverse order, and **resume** in forward order, using the same graph. A GPIO controller or power domain stays up until its last consumer is down.
- A driver can add an edge at bind time only by naming a firmware reference, never as an anonymous "try again later".

The graph logic (insertion, cycles, order, the "what blocks whom" report) is pure C. It runs and is tested on the `hosted` platform against captured device trees and ACPI dumps (§11.10).

### Power before lease (D3)

§2.4 says "drivers ask `power` for their unit before touching its registers". That is a rule enforced by convention, and the survey's class 7 is what happens when it is broken. On the CIX GPU it is an SError; on the Q8B it is a SoC reset.

**Proposed:** the hwdb asks `power` to power the node on *before* it grants the MMIO lease. A driver that holds an MMIO lease can rely on the device being powered. Runtime power management (later) must keep that true: before `power` turns a node off, the hwdb suspends the driver's MMIO grant (or the driver gives it back), and touching it then is a fault in the driver's domain, not a bus error that takes the SoC down.

### Provider state is per connection

A driver's power votes, clock enables, GPIO pin claims and interrupt subscriptions are held by servers, not by the kernel. P6 only reclaims them if each provider keys that state to the client's connection and releases it on the death notice. **Proposed rule:** every provider server keeps client state per connection and drops it when `ca_Closed` is set. This is "no state spill" (Theseus, S§9) applied across processes. A conformance test kills a driver that holds a power vote and checks the domain goes off.

## 11.6 Binding

### Who does it

**Proposed: the hwdb is the driver manager.** It already owns the graph and grants leases (§9.4). It also:

- matches nodes to drivers (D5);
- starts driver processes when their node is ready;
- supervises them and restarts them (§11.10);
- sends `STOP` and kills (§11.4).

### Matching is data

- Each driver binary carries a **match table**: the DT `compatible` strings and ACPI `_HID`/`_CID` values it supports, each with a rank. It lives in an ELF note section (`.note.planar.match`), so the hwdb can read it from the boot image without running the driver.
- **Rank** follows the firmware's own order. A DT `compatible` list runs from most to least specific, and `_HID` outranks `_CID`. The FreeBSD values (specific, vendor, default, generic) are a good scale, and a driver may lower its rank for a generic fallback, as a `GENERIC` class-code driver does.
- A build-time check (`make check`) loads every match table in the boot image and fails if two drivers match the same key at the same rank. FreeBSD resolves such ties silently by registration order.
- There is no probe that runs twice (S§2.3.6). If a driver needs to look at the hardware to decide, it does so at bind time and exits with a "decline" status. The hwdb then tries the next-ranked driver.
- Board quirks (§9.4) can pin a node to a driver or exclude one. They don't change match tables.

### Bus drivers add nodes

This settles the §9.6 question about PCIe enumeration. **Proposed:** a bus driver is an hwdb *source*, as `acpid` is. The `pci` server binds to the host bridge node, scans config space, and publishes a child node per function, with BARs as resources and the MSI capability as an interrupt resource. A USB hub driver will do the same. The hwdb stays the one graph (newbus, IOKit nubs, S§2.2, S§6.1). A function that splits into parts (a PMIC with regulators, an RTC and GPIO; a GPU's audio function) publishes child nodes the same way. That covers Linux's auxiliary bus.

### Names and units

- A device's unit number is fixed when it's matched, from enumeration order, not from the order drivers finish starting. Drivers start in parallel, and names must not depend on that (S§4.3).
- Names users see are class names, assigned by the class (§11.9), not hwdb topology paths. Fuchsia deprecated devfs because topological paths leaked implementation details (S§8).

### No deadlock across processes (D6)

A server that waits on a driver while the driver waits on it is a deadlock that no lock checker sees. The equivalent of class 8 here is an hwdb that `DoMsg`s a driver during bind while the driver's bind code `DoMsg`s the hwdb. **Rules:**

- The hwdb never blocks on a driver. Everything it sends a driver is asynchronous, and it keeps serving while it waits for a reply.
- A consumer may `DoMsg` a supplier. A supplier sends its consumers only notices (`PutMsg` or a signal), never a `DoMsg`. Since the graph has no cycles, neither do blocking calls.
- `DoMsg`'s priority loan already follows the same direction: from consumer to supplier.

## 11.7 Inside a driver: `drv.library`

WDF's lesson is that drivers got PnP, power and removal wrong until the framework wrote that code once (S§7.2). **Proposed:** a user-mode `drv.library` that every driver links. It owns the main loop, so the driver supplies callbacks:

| Callback | Runs | Does |
|---|---|---|
| `Bind(node)` | Once, at start | Takes the leases the node offers, resets and identifies the hardware, allocates DMA memory. Returns ready, decline, or an error. |
| `Start()` | After `Bind` succeeds | Publishes the class port and starts serving. |
| `Interrupt(line)` | When the line's signal arrives | Reaps completions. The library acknowledges the line afterwards. |
| `Request(msg)` | For each message on a class connection | Handles one request or a batch. |
| `Stop(reason)` | On `STOP` (§11.4) | Quiesces the hardware within the deadline. Optional: the kill path covers a driver that doesn't have it. |

The library also handles death notices on client connections, timers, `STOP`, and the deadline. A driver never writes its own `Wait` loop unless it wants to.

**Concurrency.** One task by default. A driver can ask for one task per hardware queue: NVMe I/O queue *n* gets its own task, its own MSI-X vector routed to CPU *n*, and its own set of client connections. Each task owns its queue outright, so there are no locks on the data path (sDDF is single-threaded per queue). A serial queue in front of a multi-queue device is where DriverKit lost throughput (S§6.2).

**Interrupt delivery.** Today an interrupt costs a trap, a signal, a switch to the driver task, and later an `AckIrq` trap. **Proposed:**
- `AckIrq` and the next `Wait` become one call (`AckWaitIrq`), as reply-and-wait did for IPC. The library uses it.
- For edge-triggered and MSI sources the kernel doesn't mask, so no ack is needed at all.
- A driver drains completions until the queue is empty before it re-enables device interrupts. This is the device-side version of the ring's `waiting` protocol (§4.3): under load, the device raises one interrupt per burst, not one per completion.

**Interrupts that need a bus transaction.** A GPIO interrupt on a controller behind I2C, or an ACPI GED event, can't be acknowledged by the kernel (WDF's passive-level interrupts, S§7.1). In Planar the GPIO controller's driver delivers these as signals on a connection. `drv.library`'s "get interrupt *n* of my node" returns either a kernel-bound line or a provider-delivered one, and the driver handles both the same way. These will be common on the ACPI arm64 boards.

**Shared lines.** A level-triggered line shared by drivers in different processes stays masked until every sharer has acknowledged it. Shared lines should be rare on our SoCs. On PCIe, drivers use MSI or MSI-X.

## 11.8 DMA and the data path

### Allocation follows the node

§3.3 has the right Area flags (`AREAF_DMA32`, `AREAF_NC`, `AREAF_COHERENT`), but today a driver would choose them itself. §5.2's per-device coherency table is the kind of fact that belongs in the hwdb, not in driver code. **Proposed:** `AllocDmaArea(lease, size, …)` takes the device's constraints from its node: address limit (`dma-ranges`, `_DMA`, a quirk), coherency (`dma-coherent`, `_CCA`, the §5.2 table), and the IOMMU context. The driver can only tighten them. This is busdma's lesson: constraints are inherited, not restated (S§2.2).

### Cache maintenance without a trap

On non-coherent devices, handing a buffer to the device and taking it back needs cache cleans and invalidates. If each one is a kernel call, the cost lands on every I/O. **Proposed:** let user mode do it, as `drv.library` calls `DmaSync(area, off, len, to_device | to_cpu)`.
- **RISC-V:** set `senvcfg.CBCFE` and `senvcfg.CBIE` so U-mode can run Zicbom's `cbo.clean`, `cbo.flush` and `cbo.inval` *(verify the K1 honours these)*.
- **AArch64:** set `SCTLR_EL1.UCI` so EL0 can run `DC CVAC` and `DC CIVAC`. EL0 has no invalidate-only operation, so `to_cpu` uses clean and invalidate.

Under P1 there's no attacker to stop, and the instructions only act on memory the domain can already reach.

### Device data is untrusted input

Descriptor indices, lengths and status words written by a device are checked before use, as the V2 library already checks ring indices and message pointers. A value out of range is a device error (logged, and perhaps a reset), never an out-of-bounds index. PCIe surprise removal (all-ones reads) is a case of this.

### Mappings are long-lived

Per-I/O IOMMU mapping is expensive (S§9, Asterinas). Under the identity layout, a DMA Area can be mapped into the device's IOMMU context once, when it is allocated or lent, at its own address, and left there. Drivers keep pools of preallocated DMA buffers for receive buffers and command lists, so steady-state I/O maps nothing. Lent client buffers (`IOF_LEND`) are the open case: mapping them per request costs, and bouncing them through a pool costs a copy (VQ26).

With an IOMMU, a mapping covers whole pages. DMA-visible pages hold only payload and descriptors, never message headers, pointers or library state (Thunderclap, S§12). That keeps a buggy device's overrun inside the buffer's own pages.

### Crossings per I/O (D7)

A read from an application goes app → filesystem → block driver → device, and the completion goes back. That is two connections, each of which costs a trap only when its receiver is asleep. Rough arithmetic from omega's figure: one `DoMsg` round trip is 3,361 cycles, about 2.1 µs on the X60 at 1.6 GHz. Two of them, plus an interrupt delivered to a user task, come to perhaps 5–6 µs per I/O at queue depth 1. *(This is an estimate to be measured, not a result.)* For SD and eMMC, where an access takes hundreds of microseconds, that doesn't matter. For NVMe at queue depth 1, with 10–20 µs device latency, it could be a large fraction. That matches Fuchsia's finding that the cost is worst at low queue depth.

What keeps it small:
- **Batching.** The block class protocol carries batches of requests per message and returns batches of completions (§11.9). Under load the rings stream with no traps (§4.3: 24–32 calls per 1,000 messages).
- **Handoff.** `DoMsg` already hands the CPU to the server at the caller's priority.
- **No copies.** Under the identity layout a client's buffer address is also its DMA address. The filesystem passes the app's buffer straight to the driver, and the driver hands it straight to the device. This is a concrete VQ3 benefit, and V4 should measure it.
- **CPU locality.** A per-queue driver task runs on the CPU its MSI-X vector targets, so completion signals don't need IPIs.
- **Storage queue leases (later).** NVMe has many I/O queue pairs. The filesystem could hold one as a lease, as an app holds a GPU queue (§5), and the driver would keep only the admin queue. That removes the driver from the data path completely. NVMe doorbells usually share one page, so this needs P1's trust in the filesystem. It is VQ27.

**Proposed V4 measurement:** 4 KiB random read IOPS and latency at queue depths 1, 2, 8 and 32, on NVMe and on SD. Report traps per I/O and crossings per I/O along with them.

## 11.9 Classes

A **class** is a protocol, a header in `include/planar/devices/`, and a client library, shared by all drivers of one kind. This is the IOKit family, sDDF's device class and FreeBSD's GEOM (S§13.3).

| Class | Driver serves | Client library owns |
|---|---|---|
| block | Batches of read, write, flush and trim on one queue; geometry; media change | Partitions (a multiplexer server, as sDDF's virtualiser), unit names, retries, `AbortIO` |
| serial | Line settings, transmit and receive buffers, modem lines | Line discipline, the console hand-over |
| display | Already designed (§5.3) | — |
| net (later) | Transmit and receive queue pairs, offloads, link state | The stack (VQ15) |
| input (later) | Events | Key maps |

Rules for every class protocol:
- **Each request is replied to exactly once.** `AbortIO` asks; it doesn't complete. An aborted request is still replied, with `IOERR_ABORTED` or with its real result if the device got there first. Exec's `AbortIO` had the same rule; WDM's cancellation races are what breaking it looks like (S§7.2). The class library enforces it, so drivers never write cancellation code.
- **Batch-first.** One message can carry many requests, and one reply many completions.
- **Replies are validated.** A driver is a separate fault domain, and the survey's SoK found most isolation failures were bad interfaces, not bad isolation (S§12). The class library checks lengths, counts and status codes in a driver's reply before passing them on, so a broken driver can't crash its clients.
- **`ERR_PORT_DIED` means the device went away.** The class library fails outstanding requests, reopens the unit's port when the driver comes back (§11.10), and retries requests that are safe to retry. Block reads and writes are; the filesystem decides about flushes.
- **Versioned.** Each protocol has a version in its connection header, as the arena header does.

## 11.10 Robustness and verification

**Driver restart.** A dead driver is a process exit. The hwdb restarts it (MINIX's reincarnation server), with backoff, and gives up after a few failures in a short window. The node stays in the graph and keeps its unit, so the new driver republishes the same names and clients reconnect. Stateless drivers recover completely. Stateful ones recover what their class can retry. Restart is cheap in Planar, unlike in AbyssBSD, so it belongs in V4 rather than "never" (VQ23; see [§12](12-let-it-crash.md) for port continuity and the supervision tree).

**Device reset.** A driver that finds its device wedged asks the hwdb for a reset. The hwdb unbinds (§11.4), runs the bus or power reset, and binds again. This is Windows' TDR, applied to any device (S§7.1).

**Host testing.** On the `hosted` platform:
- matching against the captured DTBs and ACPI dumps, so each node gets the expected driver;
- graph construction, cycle breaking and ordering;
- each driver's protocol logic against a fake register block. The 8250 driver's split is the pattern.

**A verifier mode in `drv.library`.** Seeded, reproducible fault injection for allocation, lease grants, DMA allocation and device timeouts, so error paths in `Bind` actually run. A check that every request was replied to exactly once.

**The kill test.** The V4 counterpart of V2 step 4's death tests and AbyssBSD's detach stress test: kill a block driver at random points under I/O, repeatedly, on QEMU and the RV2. After each cycle check that:
- the client saw `ERR_PORT_DIED` and recovered after the restart;
- no Area was freed while the device could reach it (quarantine accounting);
- the driver's power votes were released;
- no handles leaked.

As with V2, mutation tests show each check catches its bug: no quarantine, no stop method, a provider that keeps votes after death.

## 11.11 What not to take

| From | Why not |
|---|---|
| A stable binary driver ABI (illumos, IOKit, Banjo) | Freezes mistakes. Class protocols are versioned instead. Library vectors are append-only, as Exec's were. |
| Generic filter stacks (WDM) | Most of WDM's complexity. Interposition happens at class-defined points, such as the partition multiplexer. |
| Deferred probe (Linux) | A blind retry that can't say what it's waiting for. |
| Per-request threads or queues between class and driver (DriverKit, Fuchsia DFv1) | The +50–150% CPU in §11.1. Our crossings are rings that stream without traps. |
| Driver-owned teardown (FreeBSD, WDM) | Every system that left it to drivers got it wrong. |
| sysfs-style attribute files | Became an accidental ABI in Linux. Device information is a hwdb query. |
| Rust's type-level guarantees | Not available in C23. Process isolation, handles and `drv.library` give the run-time version, and the verifier and kill test check it. |

## 11.12 Effects on other documents

| Document | Change |
|---|---|
| §2.3 exit order | Step 4 runs the lease's stop method. Step 5 quarantines lease-tied Areas (§11.4). |
| §2.4 `power` | The hwdb powers a node before it grants the lease. Drivers don't sequence power (§11.5). |
| §4.6 interrupts | `AckWaitIrq`; no masking for edge and MSI sources (§11.7). |
| §9.6 | PCIe enumeration: the `pci` server is an hwdb source (§11.6). |
| §8 V4 exit | Add the kill test, the queue-depth measurements and the stop methods. |
| questions | VQ23 to VQ27. |
