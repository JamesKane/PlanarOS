# CIX P1: board dumps

Taken from the Orange Pi 6 Plus on 2026-10-20, running AbyssBSD's FreeBSD 16.0-CURRENT, for V3 (docs/v3-plan.md).

| File | What |
|---|---|
| `acpi/*.dat` | Every ACPI table the XSDT lists, plus the XSDT and DSDT, read raw from `/dev/mem`. All checksums are good. They are fixtures for `kernel/acpi.c`'s hosted tests. |
| `acpid.txt` | What acpid must find in those tables, run on the host (`make hwdb-test`, docs/v4-plan.md step 4): each device, the count of what took a method (§9.6), and the addresses the AML touched, which the host hasn't got and reads as all ones. |
| `hwdb.txt` | The report hwdb's graph must give from those tables and acpid's devices (`make hwdb-test`). |
| `hwdb-static.txt` | The same from the static tables alone, as when acpid fails (docs/v4-plan.md step 2). |
| `acpi-expect.txt` | The lines the kernel must print when it reads those tables, hosted: `make PLATFORM=hosted ACPI_BOARD=cix-p1 test` (part of `make test-all`). |
| `firmware-boot.log` | UART2 from power-on to FreeBSD's loader: TF-A, OP-TEE, UEFI. Escape sequences stripped. |
| `freebsd-dmesg.txt` | FreeBSD's boot messages: devices found through ACPI, the CPU feature registers. |
| `v6-kill-test/run-*.log.gz` | V6's exit: 10 boots in a row of `make PLATFORM=uefi-a64 test` at `9b9ba67` (2026-10-22), each through `V6.8 done` with the kill test (docs/v6-plan.md step 8). |

## Facts

**Firmware.**
- The chain is TF-A BL2/BL31 v2.7, then OP-TEE, then CIX UEFI `BiosV1.3-W20251113` (EDK2).
- The board ID is "A4 (PHECDA-Rev A)". Its OEM ID is `CIXTEK` and its OEM table ID `SKY1EDK2`.
- UEFI 2.70. It says "Dtb file not found": ACPI only.
- The UEFI Shell is Boot0002.
- It honours `BootNext` (seen 2026-10-20). An EFI application that returns lands in the boot manager, not the next boot option.
- It enters an application at EL2, with VHE on. On a core started by PSCI `CPU_ON`, EL2's trap controls are UNKNOWN: three of the A520s came up with `CNTHCTL_EL2` trapping EL1's virtual timer (docs/v3-plan.md, step 3).

**Console.**
- UART2 is a PL011 at `0x40d0000`, at 115200 8N1, and is left set up by the firmware.
- There's no SPCR. DBG2 names it: type `0x8000` (serial), subtype 3 (ARM PL011), 32-bit access, `\_SB.COM2`. The kernel uses it through a quirk entry.
- FreeBSD's own console goes to the screen, not UART2.

**The interrupt controller and the CPUs** (from the MADT):
- GICD at `0xe010000` (GICv4), GICR at `0xe090000` (length `0x300000`), ITS at `0xe050000`.
- It's a GIC-700, `GICD_IIDR` `0x0402143b` (variant 2), so erratum 2941627 applies.
- The GICC entries give no per-CPU GICR address (0) and no efficiency class (0). Core classes have to come from MIDR or the PPTT.

| ACPI UID | MPIDR | Core |
|---|---|---|
| 0 (boot) | `0xa00` | A720 |
| 1 | `0xb00` | A720 |
| 2–5 | `0x000`–`0x300` | A520 |
| 6–11 | `0x400`–`0x900` | A720 |

**Clusters and speeds** (V3 step 4):
- The PPTT has one package and five clusters, by UID `0,1 2,3,4,5 6,7 8,9 10,11`. No caches are described. The package node carries processor ID 0 with the "ID valid" flag, the same ID as UID 0's leaf.
- SSDT2 gives each processor a static `_CPC` package. Its highest performance is 8192 for UIDs 0–1, 7876 for 10–11, 7246 for 6–7, 6931 for 8–9, and 2232 for the A520s (2–5). FreeBSD attaches one CPPC frequency driver per cluster (on CPUs 0, 2, 6, 8 and 10).
- At boot the clocks are below those levels: the A720 at `a00` runs at 1.9 GHz and the A520s at 1.8 GHz, by the cycle counter. Measured, an A520 has 0.61 of the fastest A720s' speed, and the A720 grades span 18% (docs/v3-plan.md, step 4).

**Costs** (V3 step 5, identity layout), in cycles on the A720 at `a00` and the A520 at `0`:
- a null kernel call: 258 and 586;
- a domain switch: 63 and 66, with 16-bit ASIDs and no flush;
- a Signal and Wait round trip between kernel tasks: 882 and 1,666;
- a `DoMsg` round trip: 1,533 and 3,366.

**Timers.**
- The generic timer runs at 1 GHz. Its GSIVs are 29 (secure), 30 (non-secure), 27 (virtual) and 26 (EL2).
- There's no memory-mapped timer.
- The Sky1 GPT runs at 25 MHz, and is found through the DSDT only.

**Watchdog.**
- An SBSA watchdog: refresh frame `0x16008000`, control frame `0x16003000`, GSIV 408.
- Its refresh frame doesn't refresh it; refresh through WOR. FreeBSD finds it and drives it.
- It's stopped when an application starts. Its WS1 resets the board (seen with `BOOTARGS=hang`).

**CPU features.**
- Both cores are r0p1, with SVE and SVE2, BTI and MTE3.
- 16-bit ASIDs, 48-bit VA, 1 TB PA, VHE.
- PMUv3p7. SPE on the A720 only.

**Memory.** 32,592 MB of real memory, 31,830 MB available to FreeBSD. EFI's map puts RAM at:
- `0x8000_0000`–`0xffff_ffff`, with holes;
- `0x1_0000_0000`–`0x7_ffff_ffff` (28 GiB);
- `0x80_0000_0000`–`0x80_7fff_ffff` (2 GiB, at 512 GiB).

**The test rig.**
- Serial: the FT232R adapter (`/dev/cu.usbserial-ABSCDIMD`) on header pins 1/3/5. The adapter's RXD goes to pin 1. TX and RX had to be swapped from the RV2's wiring.
- Network: root over ssh with a key, at the address DHCP gives it (192.168.0.19 on 2026-10-20).
- The EFI partition is `nda0p1`, shared by FreeBSD (Boot0005, first) and Ubuntu (Boot0004). `efibootmgr` works as root.
- `make PLATFORM=uefi-a64 test` runs one boot (`tools/efi-boot.py`): the image goes to `\EFI\planar\planar.efi`, Planar's entry is `Boot0050`, and `BootNext` points to it. FreeBSD's `efivar -w` takes a value on stdin only. Its `-f` isn't implemented for writes.
- The serial port must be free while a run is in progress.

**The screen** (V5 step 3):
- The GOP drives DPU4, a Linlon-D60 (product `0x0060`), at **1920×1080, 60 Hz**: one layer (layer 0, XRGB8888, `0x10`) on its framebuffer at `0x84800000`, straight into compiz input 0. There's no scaler in the path.
- display's planes go on layers 1 and 3, through compiz inputs 1 and 2 above it. A commit reaches the screen about two frames after it's written (33 ms), where QEMU's takes one.
- The DPU isn't coherent (`_CCA` 0). What the CPU draws in cached memory must be cleaned out with `dc cvac`. Until V5 step 3, the AArch64 `drv_dma_clean` only ordered writes. With the framebuffer mapped cached, the console's pixels stayed in the cache and the top of the screen showed stale memory.

**Power** (V5 step 2):
- SCMI answers on both channels:
  - the SCP, through mailbox 6's first 0x80 bytes, rung at `+0x80`: clocks, protocol version 2.0;
  - TF-A by SMC `0xc2000001`, through `0x84380000`, with x1 = x2 = 0: power domains, version 2.1.
- The EFI map has `0x84380000` reserved.
- Of the G720's set (domain 21, `gpu_clk_core`, `gpu_clk_stacks`), one is on at boot.

**The GPU** (V5 step 5):
- The G720: `GPU_ID 0xc8700008` (arch 12), shader cores `0x550555` (10; none harvested by RCSU `0x304`), 8 address spaces, 48-bit VA. ACPI's `_CCA` is 1, but it's driven as Linux's CIX patches drive it: ACE-Lite, every page non-cacheable.
- Its windows are the RCSU (`0x15000000`) and its registers (`0x15010000`), with doorbell *n* at `+0x80000 + 0x10000 n`. Its lines are 269 (job), 270 (MMU) and 271 (GPU).
- UEFI leaves it held in reset: RST0 `0x16000400` bit 6 is clear (active low). Reading a register before releasing it is refused by the interconnect, and the CPU takes an SError.
- Pulsing the RCSU's reset (`0x16000800` bit 28) loses the non-secure access TF-A gave the RCSU ("IDM: GPU secure access" on the next access).
- After a refused access, TF-A prints IDM errors without end and the watchdog doesn't reset the board: it needs a power cycle.
- GPUP's power bits (`0x15000218` bit 12 and 2–11) are already set when Planar starts. RST0's memory-repair strap (`0x16000504` bit 4) is clear: no repair.
- `mali_csffw.bin` (arch 12.8, interface 1.1) boots with 8 group slots.

**USB** (V5 step 4):
- Ten Cadence CDNSP controllers, xHCI 1.10 with 64-byte contexts, not coherent (`_CCA` 0). UEFI leaves them in host mode, so an xHCI reset is all they need. XHC0–5 have two ports (USB 2 port 1, USB 3 port 2), USB0–3 one (USB 2).
- hwdb gives xhci their `CIXH2031` descriptions (`CUBn`), on their xHCI windows and host lines alone. The `PNP0D10` ones are aliases. Three of the latter are disabled by VQ42, but the CIXH2031s' `_STA` isn't, so all ten run.
- The rig's keyboard and mouse are on `USB1` (`/_SB_/HUB1/CUB1`, `0x9298000`, xhci's unit 7), behind a single-TT high-speed hub (1a40:0101). On it are a full-speed Sonix keyboard (0c45:0133) on port 3, and a Corsair mouse (1b1c:1bac) on port 2, which also has a keyboard interface. That's where FreeBSD saw them. Which other connector is which controller is still unknown.
