# The drivers

*Checked against V4 step 7.*

The drivers built so far, each a program in the image's boot bundle (`drv/NAME/`). Their match tables are in `servers/hwdb/drivers.c`; their stop methods in `servers/hwdb/server.c`.

| Driver | Matches | Port | Interrupts | Stop method |
|---|---|---|---|---|
| [nvme](#nvme) | `pciclass,010802` | `nvme.N` (block) | MSI-X, else polled | `CC.EN` to 0 |
| [sdhci](#sdhci) | `ky,x1-sdhci`, `spacemit,k1-sdhci`, `pciclass,080501` | `sd.N` (block) | Its line, else polled | Software Reset For All |
| [display](#display) | `simple-framebuffer` | `con.N` (console) | None | None needed: no DMA |
| [pci](04-pci.md) | `pci-host-ecam-generic`, `PNP0A08` | None | None | None needed |
| [virtio-mmio](#virtio-mmio) | `virtio,mmio`, `LNRO0005` | `rng.N` | Its line | Status register to 0 |
| [rtc-pl031](#rtcs), [rtc-goldfish](#rtcs) | `arm,pl031`, `ARMH0031`; `google,goldfish-rtc` | `rtc.N` | None | None needed |

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
- **On the K1** (the RV2), as Linux's `sdhci-of-k1` does: after a full reset it sets up the PHY and pads again and forces the card clock on; it takes the base clock from the device tree's 204.8 MHz, since the capabilities are wrong; it finds the card by asking it, since card detect is a GPIO's; and it never uses 64-bit DMA. *Not yet run on the RV2.*
- QEMU's `sdhci-pci` shows the same driver on QEMU.

## display

`drv/display`: a text console on the framebuffer UEFI's GOP left, which hwdb describes as a `simple-framebuffer` node with its geometry. Formats RGBX and BGRX, 8 bits a colour. See [chapter 3](03-console.md). Native drivers for the K1's DPU and the CIX's display controller are **Planned**, V5.

## virtio-mmio

`drv/virtio-mmio`: QEMU's virtio-mmio slots, all the slots in a page as one unit. It drives the entropy device (`rng.N`: `DRV_READ` gives up to 64 random bytes) and declines every other slot.

## RTCs

`drv/rtc-pl031`, `drv/rtc-goldfish`: real-time clocks. `DRV_READ` gives the time, seconds since 1970, as 8 bytes.
