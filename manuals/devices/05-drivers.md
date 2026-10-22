# The drivers

*Checked against V5.*

The drivers built so far, each a program in the image's boot bundle (`drv/NAME/`). Their match tables are in `servers/hwdb/drivers.c`; their stop methods in `servers/hwdb/server.c`.

| Driver | Matches | Port | Interrupts | Stop method |
|---|---|---|---|---|
| [nvme](#nvme) | `pciclass,010802` | `nvme.N` (block) | MSI-X, else polled | `CC.EN` to 0 |
| [sdhci](#sdhci) | `ky,x1-sdhci`, `spacemit,k1-sdhci`, `pciclass,080501` | `sd.N` (block) | Its line, else polled | Software Reset For All |
| [display](#display) | `ky,dpu`, `CIXH5010`, `simple-framebuffer` | `con.0` (console), `display.0` | The DPU's vblank | Every plane off but the console's, latched |
| [pci](04-pci.md) | `pci-host-ecam-generic`, `PNP0A08` | None | None | None needed |
| [virtio-mmio](#virtio-mmio) | `virtio,mmio`, `LNRO0005` | `rng.N` | Its line | Status register to 0 |
| [rtc-pl031](#rtcs), [rtc-goldfish](#rtcs) | `arm,pl031`, `ARMH0031`; `google,goldfish-rtc` | `rtc.N` | None | None needed |
| [xhci](#xhci) | `pciclass,0c0330`, `ky,x1-dwc3`, `CIXH2031` | None: a client of `input` | Its line or MSI-X, else polled | USBCMD.R/S to 0, until halted |
| [power](#power) | `ky,x1-pm-domain`, `ky,x1-ccu`, `ky,x1-reset`, `CIXHA004`, `planar,power-model` | `power` | SCMI's mailbox, else none | None needed: it keeps no DMA |
| [gpu-panthor](#gpu-panthor) | `CIXH5000` | `gpu.0` | The job's, MMU's and GPU's lines | The MCU off and a soft reset, unless unpowered or in reset |
| [gpu-rogue](#gpu-rogue) | `img,rgx` (a quirk adds the node) | `gpu.0` | Its line (75) | A soft reset of everything, the MIPS with it |

## nvme

`drv/nvme`: NVM Express controllers.
- Bind resets the controller (`CC.EN` 0, then waits for `CSTS.RDY` to clear), sets up the admin queue, identifies the controller and namespace 1, and makes one I/O queue pair, 64 deep. It reads block 0 to show the path works.
- It takes namespaces with 512 to 4096-byte blocks, and controllers whose memory pages can be 4 KiB.
- Each block request is one command, its buffer described by PRPs: a PRP list in a DMA Area when it spans more than two pages. Requests beyond the queue's 63 wait their turn in the driver. A request is at most 128 KiB, less if the controller's MDTS says so.
- Completions come by MSI-X: vector 0 for the admin queue, the last for the I/O queue. With no vectors (no ITS, as on RISC-V QEMU) it polls.

## sdhci

`drv/sdhci`: SD host controllers (SD Host Controller 3.00) and SD cards in them.
- Bind resets the controller, then takes the card through identification at 400 kHz, reads its size from the CSD, and runs it 4 bits wide at 25 MHz. SD 2.0 and later cards only: SDSC, SDHC and SDXC, not MMC or eMMC, which it declines.
- One command at a time, as the card does them: each request is a CMD18 or CMD25 with Auto CMD12, the data moved by ADMA2. ADMA2's addresses are 32 bits; a buffer above 4 GiB goes through a bounce buffer in a DMA Area below it (`AREAF_DMA32`).
- **On the K1** (the RV2), as Linux's `sdhci-of-k1` does: after a full reset it sets up the PHY and pads again and forces the card clock on; it takes the base clock from the device tree's 204.8 MHz, since the capabilities are wrong; it finds the card by asking it, since card detect is a GPIO's; and it never uses 64-bit DMA. Its DMA doesn't snoop the caches, so the driver cleans descriptors and data it hands the device and flushes what the device writes (`drv_dma_clean`, `drv_dma_flush`). It takes its interrupt (line 99) on the RV2; if a command's interrupt doesn't come within its 2-second deadline, it polls from then on and says so.
- QEMU's `sdhci-pci` shows the same driver on QEMU.

## display

`drv/display`: the screen the firmware left lit, never set up again. hwdb leases it the firmware's framebuffer, and with it the controller scanning it out, where a quirk names one (`HW_Q_SCANOUT`): the K1's HDMI DPU, the CIX's DPU4. It takes over the firmware's mode, draws the text console on plane 0, and gives clients an overlay and a cursor plane above it ([chapter 10](10-display.md)). Where there's no controller it knows, or the controller isn't lit, it composes the planes itself on the framebuffer, as on QEMU. Formats RGBX and BGRX for the console, XRGB8888 and ARGB8888 for the planes. See also [chapter 3](03-console.md).

Its stop method is in `servers/hwdb/display.c`, since a reset would blank the screen (and on the CIX lose the DisplayPort link for good): every plane off but the one on the firmware's framebuffer, then a wait for the latch. A restarted display finds the console as it was and goes on below its last line.

## xhci

`drv/xhci`: USB host controllers (xHCI 1.2), their hubs, and the boot keyboards and mice behind them, whose reports it sends to `input` ([chapter 7](07-input.md)). It publishes nothing. QEMU's is found by `pci`; the K1's DWC3 it makes a host first, with its USB 2 PHY; the CIX's ten UEFI left in host mode. Its stop method, in `servers/hwdb/input.c`, halts each controller.

## power

`drv/power`: the clocks, resets and power domains, on the K1 (its APMU and PMU) and the CIX (SCMI to the system control processor, and power domains through TF-A), and QEMU's model in hwdb's test mode. hwdb votes for a driver's resources before it grants the lease, and drops the vote only after the driver's stop method has run. A restarted `power` keeps on what it finds on ([chapter 8](08-power.md)).

## gpu-panthor

`drv/gpu-panthor`: the CIX's Mali-G720, through its CSF firmware (`DEVS/mali_csffw.bin`, read through `dos`). It gives each client a CSF group pinned to a slot, with the slot's doorbell mapped into the client: a hardware queue ([chapter 9](09-gpu.md)). Its stop method, in `servers/hwdb/gpu.c`, touches no register of a GPU that isn't powered or is held in reset: the access would wedge the board.

## gpu-rogue

`drv/gpu-rogue`: the RV2's BXE-2-32, a Rogue core with a MIPS firmware processor, running mainline's open firmware (`DEVS/rogue.fw`). U-Boot's tree has no node for it, so a quirk (`HW_Q_NODE`) adds one. Each client's ring is a firmware context's client CCB, and `GPU_KICK` asks the server to kick it: a server queue ([chapter 9](09-gpu.md)). Its stop method, in `servers/hwdb/gpu.c`, puts the GPU back in soft reset, which holds the MIPS.

## virtio-mmio

`drv/virtio-mmio`: QEMU's virtio-mmio slots, all the slots in a page as one unit. It drives the entropy device (`rng.N`: `DRV_READ` gives up to 64 random bytes) and declines every other slot.

## RTCs

`drv/rtc-pl031`, `drv/rtc-goldfish`: real-time clocks. `DRV_READ` gives the time, seconds since 1970, as 8 bytes.
