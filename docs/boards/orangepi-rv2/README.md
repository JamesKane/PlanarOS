# Orange Pi RV2: captured device tree

Taken on 2026-10-20 from the board, for hwdb's tests (docs/v4-plan.md step 2).

| File | What |
|---|---|
| `u-boot.dtb` | The tree the stock U-Boot (2022.10, in SPI flash) passes the kernel, 52,706 bytes: printed by the boot argument `dumpdtb` (`make PLATFORM=ky-x1 BOOTARGS=dumpdtb test`) and rebuilt from `build/ky-x1/serial-boot.log` with `tools/dtb-from-log.py`. |
| `hwdb.txt` | The report hwdb's graph must give from it (`make hwdb-test`). |

## Facts

- It's U-Boot's own control tree, not a Linux one: it has `binman` and `fit-images` nodes (which `dtc` calls errors), and its compatibles are the vendor's (`ky,x1-sdhci`, `ky,x1-ccu`), not Linux's `spacemit,k1-*`.
- The root compatible is just `"ky,x1", "riscv"`, so quirks keyed by it apply to any K1 board on this U-Boot. The model is `"ky x1 orangepi-rv2 board"`.
- Three SD/MMC controllers: `sdh@d4280000` (`mmc0`, the SD card, with a card-detect GPIO), `sdh@d4280800` (disabled) and `sdh@d4281000` (`mmc2`, eMMC).
- The PMIC (`ky,spm8821`) is on I2C 8 (`twsi8@d401d800`) at `0x41`.
- **Shared system controllers.** The clock controller, the reset controller and the power domains list the same register blocks: the MPMU at `0xd4050000` (also the watchdog's reset register, `0xd4051020`), the APMU at `0xd4282800` (shared again with the PCIe controller and its PHY), and others. Leases of whole pages can't give each its own.

## Booting Planar (V4)

- **From the SD card** (`make PLATFORM=ky-x1 sdcard`, with the card's FAT partition mounted, `SD_MOUNT=/Volumes/OMEGA` by default): `EFI/planar/planar.bin`, the commands in `EFI/planar/C`, the scripts in `EFI/planar/S`, and `boot.scr`. On power-on the stock U-Boot runs `boot.scr`, which boots Planar to the Shell. `EndShell` resets the board.
- **This U-Boot autoboots after 0 seconds and ignores Ctrl-C while it does.** With `EFI/planar/hold` on the card, `boot.scr` stops and U-Boot goes on to its prompt (after trying USB and the network, about 80 s). `Touch SYS:EFI/planar/hold` makes the file from the Shell; `Delete` removes it.
- **Test runs:** `make PLATFORM=ky-x1 SD=1 test` makes the hold from the Shell, resets the board, and loads the image from the card with `load mmc`. Without `SD=1` it goes over the serial line by `loadx` (about two minutes for the image), which fails if the line drops bytes, as it did for a while on 2026-10-20.
- **The screen:** U-Boot leaves a 1920×1080 framebuffer at `0x7f700000` scanning out over HDMI when a monitor is attached. Planar keeps it and draws its console there.

## Facts found running Planar

- **Power** (V5 step 2): every clock, reset and domain V5 drives is in the APMU's page (`0xd4282000`), which `power` leases alone. The GPU's domain (APMU `0xd0`, status `0xf0` bit 0) comes on by the vendor's software sequence and goes off again. SD's clocks and resets are on from U-Boot, so power keeps them.
- **The screen** (V5 step 3): the HDMI DPU at `0xc0440000` (the dpu node's second window), U-Boot's layer on RDMA 3, 1920×1080 at 60 Hz. display moves that layer's composer slot from 0 to 7 and splits RDMA 2's and 3's shared line buffer; neither shows. Its vsync is line 139. A commit is on the screen one frame later (16.5 ms).
- **USB** (V5 step 4): the USB-A ports behind the onboard Genesys hub (05e3:0610 and :0620, its USB 2 and USB 3 halves) are the DWC3's at `0xc0a00000`, line 125, a 3.30a core. It needs the combo PHY started even with only USB 2 walked: without its pipe clock an xHCI reset never ends. The hub's ports get VBUS from GPIO 123, which U-Boot leaves low (its tree says 79), so U-Boot itself finds nothing behind the hub; `power` drives it in the set `"usb"`. GPIOs keep their levels across a reset, after which U-Boot finds the keyboard and polls it as console input, and `loadx` crawls: `tools/serial-boot.py` stops U-Boot's USB first. A low-speed keyboard and mouse work through the hub's TT. The other USB-A port is the OTG controller's (`udc@c0900100`, an EHCI), with VBUS on GPIO 126: not driven yet.
- **Svpbmt works** on the X60, though U-Boot's ISA string doesn't list it: the kernel's probe maps uncached memory (V5 step 1).

- **DMA isn't cache-coherent** on the K1 (Linux: `dma-noncoherent` on the SoC bus). The X60 has Zicbom with 64-byte blocks; drivers clean and flush through it.
- **The PLIC ignores a completion of a disabled source**, as the spec allows: complete before disabling, and again after enabling (the vendor's Linux does).
- The SD card here is a 32 GB SDHC (61,952,000 blocks), MBR, one FAT32 partition (label `OMEGA`). The eMMC slot is empty.
