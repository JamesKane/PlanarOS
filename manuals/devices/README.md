# Devices

*Planar Reference Manuals, volume 2.* How programs do I/O, and how a driver is written.

*Checked against V4 step 7.*

**Differs from the RKM.** The Amiga's devices were libraries in one address space: a program filled an `IORequest` and called `DoIO` or `SendIO`, and the device ran in its own task in the same memory. Planar's drivers are processes, each in its own protection domain (§11). A program reaches one over a connection to the driver's **class port**, and the request is a message: a **class** fixes its layout, as the block class does. There is no `IORequest`, `DoIO`, `SendIO` or `AbortIO`. Synchronous I/O is `DoMsg`; asynchronous is `PutMsg` and waiting for the reply. A class's client library wraps it (`lib/blk`, `con_*` in `dos.library`).

The design is in [docs §9](../../docs/09-hardware-description.md) and [§11](../../docs/11-driver-model.md); what was built, step by step, is in the [V4 plan](../../docs/v4-plan.md).

## Chapters

| # | Chapter | State |
|---|---|---|
| 1 | [Drivers: hwdb, leases, drv.library, supervision](01-drivers.md) | Written, V4 |
| 2 | [The block class: NVMe, SD and lib/blk](02-block.md) | Written, V4 |
| 3 | [The console class](03-console.md) | Written, V4 |
| 4 | [PCI: a bus driver that describes what it finds](04-pci.md) | Written, V4 |
| 5 | [The drivers](05-drivers.md): nvme, sdhci, display, pci, virtio-mmio, the RTCs | Written, V4 |
| 6 | Timer as a device | **Planned**: a program has `ArmTimer` ([exec](../autodocs/exec.md#execarmtimer)) |
| 7 | Input (USB keyboards) | **Planned**, V5 (v4-plan decision 1) |
| 8 | Power and clocks (`power`, SCMI, the K1 PMU) | **Planned**, V5 |
| 9 | GPU and NPU queue leases | **Planned**, V5 ([docs §5](../../docs/05-coprocessors.md)) |

Files are the [DOS](../dos/README.md) volume's.
