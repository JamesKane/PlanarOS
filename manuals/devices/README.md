# Devices

*Planar Reference Manuals, volume 2.* How programs do I/O, and how a driver is written.

*Checked against V4; chapter 8 against V5 step 2, chapter 10 against step 3, chapter 7 against step 4, chapter 9 against step 5.*

**Differs from the RKM.** The Amiga's devices were libraries in one address space: a program filled an `IORequest` and called `DoIO` or `SendIO`, and the device ran in its own task in the same memory. Planar's drivers are processes, each in its own protection domain (§11). A program reaches one over a connection to the driver's **class port**, and the request is a message: a **class** fixes its layout, as the block class does. There is no `IORequest`, `DoIO`, `SendIO` or `AbortIO`. Synchronous I/O is `DoMsg`; asynchronous is `PutMsg` and waiting for the reply. A class's client library wraps it (`lib/blk`, `con_*` in `dos.library`).

The design is in [docs §9](../../docs/09-hardware-description.md) and [§11](../../docs/11-driver-model.md); what was built, step by step, is in the [V4 plan](../../docs/v4-plan.md).

## Chapters

| # | Chapter | State |
|---|---|---|
| 1 | [Drivers: hwdb, leases, drv.library, supervision](01-drivers.md) | Written, V4 |
| 2 | [The block class: NVMe, SD and lib/blk](02-block.md) | Written, V4 |
| 3 | [The console class](03-console.md) | Written, V4 |
| 4 | [PCI: a bus driver that describes what it finds](04-pci.md) | Written, V4 |
| 5 | [The drivers](05-drivers.md): nvme, sdhci, display, pci, virtio-mmio, the RTCs | Written, V4; display V5 |
| 6 | Timer as a device | **Planned**: a program has `ArmTimer` ([exec](../autodocs/exec.md#execarmtimer)) |
| 7 | [The input class: `input`, the ring, xhci](07-input.md) | Written, V5 step 4 |
| 8 | [The power class: `power`, the K1, SCMI](08-power.md) | Written, V5 step 2 |
| 9 | [The GPU class: an app's own queue, gpu-panthor](09-gpu.md) | Written, V5 step 5 (the RV2's `gpu-rogue`: step 6) |
| 10 | [The display class: planes, commits, the sync page](10-display.md) | Written, V5 step 3 |

Files are the [DOS](../dos/README.md) volume's.
