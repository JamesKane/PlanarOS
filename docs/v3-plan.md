# V3 plan: the first Arm board, ACPI

**Status: in progress (2026-10-20).** Step 1 done. The open decisions are at the end.

**Goal (§8):** the AArch64 port on real hardware. The board is the Orange Pi 6 Plus (CIX P1). It boots through UEFI and describes itself only through ACPI.

**Board facts are in [boards/cix-p1](boards/cix-p1/README.md)**: the raw ACPI tables (fixtures for step 2), the firmware's serial log and FreeBSD's dmesg, all taken on 2026-10-20. They confirm or settle what this plan assumed: GICD `0xe010000`, GICR `0xe090000`, UART2 from DBG2, the boot CPU at MPIDR `0xa00`, 16-bit ASIDs, the watchdog frames, and a 1 GHz timer.

**What's already built.** The AArch64 port runs on QEMU: the EL2→EL1 drop, GICv3, the generic timer, PSCI SMP (over SMC or HVC, read from the device tree), PL011, lazy FP state and both layouts. All of it is found through the FDT. V3 adds:
- the UEFI entry;
- ACPI as a second source of the same facts;
- the board's hazards;
- the Arm cost measurements and the VQ3 decision.

**Exit:**
- The V1 and V2 suites pass on all 12 cores of the CIX P1, in both layouts. The board is booted through UEFI, with ACPI only.
- The same suites pass on QEMU in three ways: through edk2 with ACPI, through edk2 with the FDT, and through `-kernel` with the FDT as today.
- On the A720 and the A520, these costs are measured and compared with the RV2: the null kernel call, the domain switch, the Signal/Wait round trip and `DoMsg`.
- **VQ3 is decided**, from data from the RV2 and the CIX.
- The manuals are current (roadmap rule).

Each step ends with `make test-all` green and a `V3.n done` line in the transcript.

| Step | Content |
|---|---|
| **1. UEFI entry, on QEMU** ✅ | The image is also an EFI application. A stub takes the memory map and the configuration tables, exits boot services and enters the kernel. It runs under edk2 on QEMU, at EL1 and at EL2. |
| **2. ACPI, on QEMU** | `kernel/acpi.c` parses the static tables into the same facts `fdt.c` gives. Both sources are compared on one machine. |
| **3. The board** | One UEFI image for the CIX: serial, the memory map, the GIC-700 and the watchdog, then one core through the suites, then all 12. Both layouts. |
| **4. Big and little** | Core classes from the PPTT and MIDR, and costs per class. |
| **5. Arm costs** | The profiler on AArch64, a lean kernel-call path like the RV64 one, and the numbers on both core types. |
| **6. VQ3** | The `vq3` workload on the CIX, again on the RV2, and switch and TLB-miss costs per layout. Then the decision. |

## 1. UEFI entry, on QEMU ✅

**Done (2026-10-20).** `make test-all` runs 9 configurations, two of them through edk2: EL1 with the identity layout, and EL2 with the global one. A 30-boot soak through edk2 at EL2 had no failures. The transcript ends with `V3.1 done`.

- **The image runs wherever it's loaded.** This wasn't in the plan, but one image needs it: the old fixed load address, `0x40200000`, is PCI space on the CIX.
  - The AArch64 kernel is built position-independent and linked as if loaded at physical 0.
  - Once the MMU is on, `entry.S` adds the physical load address to each of its 126 relative relocations.
  - The build refuses any other kind of relocation.
  - QEMU's `-kernel` path now relocates too, so every boot exercises it.
- **The image is also a PE/COFF EFI application.**
  - Its first word is `MZ`, a harmless `ccmp`. The PE header follows the arm64 `Image` header.
  - It has two sections, code and data, and file offsets equal addresses, so the flat `planar.bin` is the PE file.
  - It has no base relocations.
- **The stub** (`arch/aarch64/efi.c`):
  1. prints through UEFI's console;
  2. copies the image to a 2 MiB boundary below 512 GiB;
  3. fills the boot-information block (`kernel/bootinfo.h`): the DTB and RSDP from the configuration table, the GOP framebuffer, and the load options as the command line;
  4. calls `ExitBootServices`, taking the map again if the key changed;
  5. cleans the image to memory, turns the MMU off at its EL, and enters through `entry.S` like any loader.

  It runs before relocation, so it uses PC-relative addresses only. The build checks its object for absolute relocations.
- **The memory map comes from EFI** when there is one (`memmap.c`):
  - conventional and boot-services memory are free;
  - loader, runtime, ACPI and persistent memory are RAM but reserved;
  - adjacent ranges are merged before rounding to 2 MiB.
- **On QEMU, `EFI=1` instead of a new platform.** It's the same image, so `make PLATFORM=qemu-virt-a64 EFI=1` boots it under `edk2-aarch64-code.fd` with `acpi=off`.
  - edk2 loads it from `-kernel` through fw_cfg, with `-append` as its load options. This replaces the FAT directory the plan proposed: it's quicker, and it passes arguments.
  - `QEMU_EL2=1` runs edk2, and so the stub, at EL2.
- **`V3.1 boot`** checks that every range the EFI map keeps, and the boot block, is reserved. On EFI runs, boot-check also requires the line to say it came through UEFI, with the `layout=` argument intact.
- **Mutation tests (all caught):**
  - no relocation;
  - no boot block;
  - loader data or runtime data treated as free;
  - the command line dropped.
- **Not checked on QEMU:** the cache maintenance (QEMU has no caches to get wrong), and VHE at EL2 (edk2 doesn't enable it). Both are first tested on the CIX, in step 3.

## 2. ACPI, on QEMU

`kernel/acpi.c` walks the static tables only and never runs AML (VQ7, D1). docs/09 estimates 500–800 lines.

- **RSDP → XSDT:** with checksums. Every length is bounded by its table, with the same hardening as the device-tree parser after review M14. It's tested against broken tables, as `fdt.c` is.
- **MADT:**
  - the GICD;
  - the redistributors, as GICR ranges or per-CPU GICC entries. The stride comes from the table: 0x40000 for v4.1 frames on the CIX;
  - the CPUs, by MPIDR, with their enabled flags.

  The ITS is recorded for V4.
- **GTDT:** the timer PPIs, and the SBSA watchdog's frames.
- **SPCR, then DBG2:** the console. The CIX has no SPCR. Its DBG2 names UART2 at `0x40d0000`.
- **PPTT:** clusters and core classes (step 4).
- **MCFG:** recorded for V4.
- **Choosing the source:** a `hw=acpi|fdt` boot argument chooses the source when both exist. Otherwise the kernel uses whichever it was given. When it was given a DTB, the DTB wins (docs/09 R6).
- **The comparison test:** QEMU boots once from each source. The kernel prints its facts in one canonical form: memory ranges, CPUs, GIC, timer and console. boot-check fails if the two lists differ.
- **Board quirks are data**, keyed by the ACPI OEM ID and OEM table ID (docs/09 §9.4). The first entries are the CIX's DBG2 fallback and its RSVL ranges, for whenever audio or the DSP is driven.

## 3. The board

**One image for every UEFI Arm board.** The CIX gets quirk entries, not a platform of its own (proposed; see the decisions below). The order of bring-up:

1. A line from the stub, through UEFI's console.
2. The kernel's banner on UART2: a PL011, `ARMH0011`, at 3.3 V on the 10-pin header (pin 1 TX, pin 3 RX, pin 5 GND). UEFI leaves it programmed at 115200 (seen on the board), so the kernel writes to it without changing the baud rate.
3. The MMU and the memory map. The board has 32 GB, and the RAM ranges come from EFI. The direct map has to cover them.
4. **The watchdog.** The GTDT's SBSA watchdog is disabled at boot if it's running. Its refresh frame is broken (writing WRR doesn't refresh it), so it is refreshed through WOR if it is ever armed.
5. **The GIC-700.** It needs erratum 2941627's workaround, matched on IIDR as Linux does: an SPI whose affinity excludes this CPU is EOI'd, then `dsb sy`, then deactivated through `GICD_ICACTIVER`, not `ICC_DIR`.
6. The generic timer at 1 GHz. **Idle is WFI only**, so the per-core timers never stop and the CIX GPT isn't needed (the GPT is deferred, below).
7. One core through the V1 and V2 suites.
8. PSCI 1.1 over SMC, then all 12 cores. Their MPIDRs come from the MADT, which lists them in a different order from the device tree. The boot CPU is an A720.
9. Both layouts, then a soak.

**SVE.** The A720 and A520 have SVE2. In V3:
- user code is built without SVE;
- an SVE instruction is an illegal instruction, as it is now;
- CPTR_EL2 and ZCR are set before the drop to EL1, so the trap lands at EL1 (review M1).

Lazy SVE state is later work.

**The rig (decided 2026-10-20).** The RV2's USB-serial cable (3.3 V) moves to the 10-pin debug header:
- the adapter's RXD to pin 1 (UART2_TX);
- its TXD to pin 3 (UART2_RX);
- GND to pin 5;
- 5 V is left unconnected.

UART2 carries the firmware's log as well as ours, so the first boot shows UEFI's baud rate (expected 115200). Images reach the board as in decision 1 (a).

While the cable is on the CIX, the RV2 has no console and no serial boot. Step 6's RV2 runs need it moved back, or a second adapter.

## 4. Big and little

- **What the CIX has:** eight A720s, at four clock grades from 2.2 to 2.6 GHz, and four A520s at 1.8 GHz. The A520 does 0.39 of an A720's work per clock. VQ8 asks for placement by core class.
- **In V3:**
  - each CPU gets a class, from the PPTT and MIDR;
  - the costs are measured per class;
  - placement prefers big cores and spills onto little ones.
- **Later:** a real policy (which servers go where, and energy) waits until V4 has servers to place.

## 5. Arm costs

- **The profiler** (`make PROFILE=1`) on AArch64, through `PMCCNTR_EL0`.
- **An AArch64 kernel-call fast path**, like RV64's. It saves only what the calling convention keeps, and zeroes the scratch registers on the way out. `callregs` already checks the contract.
- **Measured on an A720 and an A520:**
  - the null kernel call;
  - the domain switch (ASID width read from `ID_AA64MMFR0`);
  - the Signal/Wait round trip;
  - `DoMsg`.

  The results go in a table next to the RV2's.
- **The parked lock-pair work** (docs/v2-plan.md, last section) is measured on both ISAs once this step is done.

## 6. VQ3

- **On the CIX:** `make vq3` (32 GB, with sizes scaled).
- **On the RV2, again:** its allocation times were dominated by a byte-at-a-time `memset`, and V2.5 fixed that.
- **New measurements on both boards:**
  - the context-switch cost per layout;
  - TLB misses per layout, from PMU refill events. QEMU can't measure these.
- **The decision** is written into questions.md and vq3-results.md.

## Deferred out of V3

- **The CIX GPT:** only needed with power-down idle (`_LPI` 1 and 2). AbyssBSD notes those states also need care with context-lost flags and EL2 state on resume.
- **Runtime services:** `GetTime`, and variables.
- **Devices for later phases:**
  - the ITS and MSIs (V4, with PCIe);
  - the SMMUs (V4);
  - the GOP framebuffer as a console (V4, display).
- **BTI and PAC** (VQ13).
- **Lazy SVE state.**

## Decisions

1. **How kernels reach the board.** AbyssBSD worked over ssh with power cycles by hand. The options are:
   - (a) Copy the image to the ESP from Ubuntu over ssh, set `BootNext`, and reboot. This is AbyssBSD's flow;
   - (b) UEFI HTTP or PXE boot from the Mac, if the firmware has it (not known);
   - (c) USB stick swaps.

   Any of them wants a 3.3 V USB-serial adapter on UART2 for the console, and a way to cut power on a hang: a smart plug, or the watchdog left armed by us.
   **Decided (2026-10-20): (a), from FreeBSD rather than Ubuntu.** The board boots AbyssBSD's FreeBSD first (Boot0005), with root over ssh. A test copies the image to the EFI partition (`nda0p1`), sets `BootNext` with `efibootmgr -n`, and reboots; the next boot falls back to FreeBSD. Serial works on UART2 (step 3). Whether this firmware honours `BootNext` is still to be checked.
2. **One UEFI image, or a CIX platform.** *Proposed:* one `uefi-a64` image, with board differences as quirk data keyed by ACPI IDs. This is the point of ACPI. The RK3588 will still use `booti` and the FDT.
3. **What VQ3's decision does to the losing layout:** keep it behind `layout=` as an option, or delete it. *Proposed:* delete it, after V3, in its own commit, so the decision shows in the history.
