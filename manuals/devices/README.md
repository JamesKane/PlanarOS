# Devices

*Planar Reference Manuals, volume 2.* How programs do I/O, and one chapter per device.

**Planned. Nothing in this volume is built yet.** The design is in [docs §4.7](../../docs/04-ipc.md), [§5](../../docs/05-coprocessors.md), [§9](../../docs/09-hardware-description.md) and [§11](../../docs/11-driver-model.md); devices arrive with V4 ([roadmap](../../docs/08-roadmap.md)).

## Expected chapters

| # | Chapter | Depends on |
|---|---|---|
| 1 | The I/O model: `IORequest`, `DoIO`, `SendIO`, `AbortIO`, `CheckIO`, over connections | `exec.library`; `IOF_LEND` |
| 2 | Serial and console | The console driver as a server |
| 3 | Timer | A timer device on the generic timer / `mtime` |
| 4 | Block devices: SD/eMMC and NVMe | V4 drivers |
| 5 | Display and framebuffer | K1 DPU, Linlon (V4) |
| 6 | Input | Later |
| 7 | GPU and NPU queue leases | V5 |
| 8 | Writing a driver server: `hwdb` binding, `AckIrq`, Areas for MMIO and DMA | V4 |

Each chapter will follow the RKM's device chapters: what the device does, opening it, the commands it accepts, errors, and an example.
