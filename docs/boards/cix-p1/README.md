# CIX P1: board dumps

Taken from the Orange Pi 6 Plus on 2026-10-20, running AbyssBSD's FreeBSD 16.0-CURRENT, for V3 (docs/v3-plan.md).

| File | What |
|---|---|
| `acpi/*.dat` | Every ACPI table the XSDT lists, plus the XSDT and DSDT, read raw from `/dev/mem`. All checksums are good. They are fixtures for `kernel/acpi.c`'s hosted tests. |
| `acpi-expect.txt` | The lines the kernel must print when it reads those tables, hosted: `make PLATFORM=hosted ACPI_BOARD=cix-p1 test` (part of `make test-all`). |
| `firmware-boot.log` | UART2 from power-on to FreeBSD's loader: TF-A, OP-TEE, UEFI. Escape sequences stripped. |
| `freebsd-dmesg.txt` | FreeBSD's boot messages: devices found through ACPI, the CPU feature registers. |

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
