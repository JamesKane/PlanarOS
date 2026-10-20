# V3 plan: the first Arm board, ACPI

**Status: in progress (2026-10-20).** Steps 1 to 5 done. All three decisions are made (end of this plan).

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
| **2. ACPI, on QEMU** ✅ | `kernel/acpi.c` parses the static tables into the same facts `fdt.c` gives. Both sources are compared on one machine. |
| **3. The board** ✅ | One UEFI image for the CIX: serial, the memory map, the GIC-700 and the watchdog, then one core through the suites, then all 12. Both layouts. |
| **4. Big and little** ✅ | Core classes from the PPTT and MIDR, and costs per class. |
| **5. Arm costs** ✅ | The profiler on AArch64, a lean kernel-call path like the RV64 one, and the numbers on both core types. |
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

## 2. ACPI, on QEMU ✅

**Done (2026-10-20).** `make test-all` runs 12 configurations, then `make compare-hw`. Two of them boot QEMU through edk2 from ACPI alone: EL1 with the identity layout, and EL2 with the global one. A 30-boot soak of the second had no failures. The transcript ends with `V3.2 done`.

- **`kernel/acpi.c`** (490 lines) reads the RSDP, the XSDT and every table it lists, then answers questions: `acpi_gic`, `acpi_cpu`, `acpi_timer`, `acpi_watchdog`, `acpi_console`, `acpi_psci` (from the FADT) and `acpi_pci` (the MCFG).
  - A table is used only if it's whole, inside the direct map, at most 16 MiB long, and its checksum is good. The RSDP must be revision 2 or later, with both checksums good.
  - Every structure inside a table must fit in it, and every walk moves forward.
  - Every read is a byte at a time. The tables are read before RAM is mapped as normal memory, and an unaligned load from Device memory faults. ACPI's fields are unaligned: the XSDT's 64-bit entries start at offset 36.
- **`kernel/hw.c` chooses the source.** `hw=acpi` picks ACPI when both are present; otherwise the device tree wins. The losing source is forgotten. Each consumer asks `hw_source()`: the console, the CPU list, the GIC's probe, PSCI and the timer.
  - On QEMU, edk2 passes ACPI or the device tree, never both. So the choice is tested in the self-test, not by a boot.
- **The redistributors** are found by walking each range the firmware gives (MADT GICR entries, per-CPU GICC addresses, or the DT's `#redistributor-regions`), and never past a range's end.
  - The stride comes from each frame's `GICR_TYPER.VLPIS`: 0x40000 with VLPI frames (GICv4, the CIX), else 0x20000. The MADT has no stride field.
- **The comparison.** Every boot prints its facts as `hw:` lines: RAM, the CPUs (sorted), the interrupt controller, the timer's interrupt and the console. boot-check saves them, and `make compare-hw` requires the `-kernel` boot, the edk2 device-tree boot and the edk2 ACPI boot to agree (5 lines on QEMU).
- **The CIX's own tables, on the host.** `make PLATFORM=hosted ACPI_BOARD=cix-p1 test` runs the kernel on `docs/boards/cix-p1/acpi/*.dat`, under a new XSDT. The transcript must contain every line of `acpi-expect.txt`: the GICv4 at `0xe010000`, 12 CPUs in the MADT's order, the timer PPIs 30/27/26, the watchdog, UART2 from the DBG2, PSCI over SMC, and the quirk entry.
- **Quirks** are a table in `acpi.c`, keyed by the XSDT's OEM ID and OEM table ID. The CIX entry (`CIXTEK`/`SKY1EDK2`):
  - takes the console from the DBG2 (elsewhere only the SPCR names the console);
  - reserves the DSDT's three RSVL ranges.
- **`V3.2 acpi`** checks the choice of source in 5 cases. It then checks the parser against tables it builds: a good set, read in full, and 13 broken sets, each refused or read only as far as it is good:
  - an RSDP with a bad checksum or revision 0;
  - a bad table checksum;
  - XSDT entries outside the direct map, or a partial last entry;
  - MADT entries of length 0, past the end, or too short for a GICC;
  - GTDT timer lists with a huge count, an entry too short to move past, or a start past the end;
  - a table length over the limit, or under the header.
- **Also changed:**
  - `fdt_init` forgets the old tree when it refuses a new one, so a refused or dropped tree is never half-read;
  - the device-tree self-test runs on every boot, with or without a tree;
  - CPUs the hosted build can't start are dropped.
- **Mutation tests:** 21 of 23 caught, by the hosted CIX run, QEMU boots and `compare-hw`. The cases that first got through led to stronger tests: the GTDT start past the end, the 3-byte GTDT entry, a flip in the RSDP's reserved bytes, and an over-long table (caught on QEMU only).
  - **Two checks can't be caught.** The RSDP's 20-byte checksum covers bytes the extended checksum also covers. The bound on a redistributor walk isn't reached on QEMU, because QEMU marks the last frame in each range.
- **Left for later steps:** the PPTT (step 4); MCFG and the ITS are only recorded (V4); the GTDT watchdog is read, but nothing drives it yet (step 3).

### The plan, as written before the step

`kernel/acpi.c` walks the static tables only and never runs AML (VQ7, D1). docs/09 estimates 500–800 lines.

- **RSDP → XSDT:** with checksums. Every length is bounded by its table, with the same hardening as the device-tree parser after review M14. It's tested against broken tables, as `fdt.c` is.
- **MADT:**
  - the GICD;
  - the redistributors, as GICR ranges or per-CPU GICC entries. The stride is 0x40000 for v4.1 frames on the CIX (as built: from `GICR_TYPER`, not the table);
  - the CPUs, by MPIDR, with their enabled flags.

  The ITS is recorded for V4.
- **GTDT:** the timer PPIs, and the SBSA watchdog's frames.
- **SPCR, then DBG2:** the console. The CIX has no SPCR. Its DBG2 names UART2 at `0x40d0000`.
- **PPTT:** clusters and core classes (step 4).
- **MCFG:** recorded for V4.
- **Choosing the source:** a `hw=acpi|fdt` boot argument chooses the source when both exist. Otherwise the kernel uses whichever it was given. When it was given a DTB, the DTB wins (docs/09 R6).
- **The comparison test:** QEMU boots once from each source. The kernel prints its facts in one canonical form: memory ranges, CPUs, GIC, timer and console. boot-check fails if the two lists differ.
- **Board quirks are data**, keyed by the ACPI OEM ID and OEM table ID (docs/09 §9.4). The first entries are the CIX's DBG2 fallback and its RSVL ranges, for whenever audio or the DSP is driven.

## 3. The board ✅

**Done (2026-10-20).** The V1 and V2 suites pass on all 12 cores of the CIX P1, in both layouts, booted through UEFI with ACPI only: `make PLATFORM=uefi-a64 test`, with `LAYOUT=global` for the other layout. A soak of 20 boots, 10 in each layout, had no failures. The transcript ends with `V3.3 done`, then the kernel resets the board and FreeBSD comes back.

- **The loop (decision 1) works without anyone at the board.** `tools/efi-boot.py` waits for the board's FreeBSD, copies the image to `\EFI\planar\planar.efi`, writes a `Boot0050` entry whose load options are the kernel arguments, sets `BootNext` and reboots. It then watches UART2. The firmware honours `BootNext`.
  - FreeBSD's `efibootmgr` can't set load options, so the entry is written with `efivar -w`, which takes the value on stdin (its `-f` isn't implemented for writes).
  - An EFI application that returns lands in this firmware's boot manager, not the next boot option. So the first-boot probe, `stub=reset`, prints the memory map and the arguments and then resets through `ResetSystem`.
  - With `reboot`, a failed test resets the board too.
- **`PLATFORM=uefi-a64`** (`mk/uefi-a64.mk`, `plat/uefi/`) is the one image of decision 2: nothing in it names the CIX. The board's differences are the ACPI quirk entry from step 2.
- **Three A520s hung in their first instructions at EL1** (MPIDR 100, 200, 300), at the first virtual-timer access, silently. The EL2-to-EL1 drop ORed its bits into `CNTHCTL_EL2` and kept the rest. On a core just powered on, the rest is UNKNOWN, and with FEAT_ECV it includes traps of EL1's virtual timer to EL2, which has no vectors. The drop now writes every EL2 control that can trap EL1 whole:
  - `CNTHCTL_EL2` and `MDCR_EL2` (every PMU counter to EL1);
  - the fine-grained traps (FEAT_FGT) and `HCRX_EL2` (FEAT_HCX), where present;
  - `ICC_SRE_EL2`, so EL1 can use the GIC's system registers.
  - Each secondary records how far it got, and the boot CPU prints the step when one doesn't come online. That is how this was found.
- **RAM at 512 GiB.** The EFI map has 2 GiB at `0x80_0000_0000`, just past the direct map. The boot tables and the direct map now cover 1 TiB (two level-1 tables), and all 32,590 MiB is managed.
- **The watchdog** (`drivers/sbsa_wdt.c`) is stopped when the kernel starts. The kernel arms it rather than leaving it off: 10 s to WS0, a reset 10 s later, refreshed through WOR every 2.5 s from a kernel timer. A hang then resets the board: `BOOTARGS=hang` checks that (decision 1 asked for a way to cut power on a hang).
- **The GIC is a GIC-700, IIDR `0x0402143b`** (variant 2), which matches erratum 2941627. When an SPI is EOI'd on a CPU it isn't routed to, the EOI is followed by `dsb sy` and a write to `GICD_ICACTIVER`. The kernel routes each SPI to one CPU and doesn't move it, so the case doesn't arise yet.
- **A test that assumed less RAM.** The `areas` program checks that more memory than there is gets refused, and asked for 31 GiB. On the CIX, with all of its RAM mapped, 31 GiB is less than what's free. The global layout backs an Area page by page, so the request ran past the test's timeout, in every global boot of the first soak. It now asks for 32 GiB, the largest size `AllocArea` takes. A board with more than 32 GiB free will need a call that reports free memory.
- **`V3.3 board`** fails if a CPU the firmware listed isn't online, or if RAM the firmware gave is left out of the direct map. The first boots lost three cores and 2 GiB that way while passing V1.1.
- **First numbers** (identity layout): an IPI round trip 894 ns; Signal and Wait across CPUs 3.5 µs, on one CPU 456 ns; a `DoMsg` round trip about 1.1 µs. Step 5 measures these properly, per core type.

### The plan, as written before the step

**One image for every UEFI Arm board.** The CIX gets quirk entries, not a platform of its own (decision 2). The order of bring-up:

1. A line from the stub, through UEFI's console.
2. The kernel's banner on UART2: a PL011, `ARMH0011`, at 3.3 V on the 10-pin header (pin 1 TX, pin 3 RX, pin 5 GND). UEFI leaves it programmed at 115200 (seen on the board), so the kernel writes to it without changing the baud rate.
3. The MMU and the memory map. The board has 32 GB, and the RAM ranges come from EFI. The direct map has to cover them.
4. **The watchdog.** The GTDT's SBSA watchdog is disabled at boot if it's running. Its refresh frame is broken (writing WRR doesn't refresh it), so it is refreshed through WOR if it is ever armed. (Built differently: it's armed, above.)
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

## 4. Big and little ✅

**Done (2026-10-20).** Every CPU has a core class. Each class has a capacity measured at boot, and placement fills big cores first and spills onto little ones. On the CIX: `planar: 2 core classes`, the eight Cortex-A720s at 1024 and the four Cortex-A520s at 626. Both layouts pass on the board (a soak of 10 boots, 5 in each layout, had no failures). The transcript ends with `V3.4 done`.

- **The core type** is `arch_cpu_type()`, read by each CPU on itself as it comes up: MIDR_EL1's implementer and part number on AArch64, and `marchid` (through SBI) on RISC-V. CPUs of one type are one class (`kernel/cpuclass.c`). AArch64 names the Arm parts it knows; QEMU's rv64 shows as `core type 2a`.
- **The PPTT gives clusters** (`acpi_cluster`): the leaf node for each UID, and its parent. On the CIX that's five clusters, `0,1 2,3,4,5 6,7 8,9 10,11` by UID; the four A520s are one cluster. The CIX's package node carries processor ID 0, as UID 0 does, so a node is taken as a CPU only if it's flagged a leaf or is nobody's parent. Clusters are a cross-check for now: `V3.4 classes` fails if one holds two core types. The device tree's `cpu-map` isn't read yet. It comes with the first device-tree board with big and little cores (the RK3588), and `capacity-dmips-mhz` is no longer read.
- **Capacity is measured, not read.** The CIX gives speeds only in AML (`_CPC` in SSDT2; static packages, docs/09 §9.6), which the kernel doesn't run. The boot CPU has each CPU in turn time one fixed workload, best of 5 runs with interrupts off: a sort, a hash and a walk of a linked cycle, about 110 µs on an A720. A class's capacity is its median time against the fastest class's, which is 1024.
  - **One capacity per class, not per CPU.** The A720s come in four clock grades, but per-CPU numbers would make placement on a symmetric machine follow timing noise. The spread is printed, per CPU, in the boot log.
  - **The measurement disagrees with `_CPC`.** `_CPC`'s highest performance puts an A520 at 0.27 of the fastest A720 (2232 against 8192); the workload gives 0.61. The firmware leaves the clocks below their maximum. By the cycle counter, the A720 at `a00` runs at 1.9 GHz and the A520s at 1.8 GHz. The grades don't follow `_CPC` either:

    | UIDs | MPIDR | `_CPC` highest | Workload |
    |---|---|---|---|
    | 0, 1 | a00, b00 | 8192 | 109.8 µs |
    | 10, 11 | 800, 900 | 7876 | 109.6 µs |
    | 6, 7 | 400, 500 | 7246 | 115.8 µs |
    | 8, 9 | 600, 700 | 6931 | 130.2 µs |
    | 2–5 | 0–300 (A520) | 2232 | 189.2 µs |

    So until a power server sets the clocks (V4, SCMI), the measurement is the honest number. Per clock, an A520 does 0.61 of the work of the A720 at `a00` on this workload, and 0.53 on a Signal and Wait round trip. That is more than the 0.39 this plan assumed: the workload is mostly dependent loads and branches, where a wide core gains least.
- **Placement** (`place()` and `nudge_idle()` in `kernel/sched.c`):
  - An idle CPU beats a busy one, and a faster idle one beats a slower one. So big cores fill first, and work spills onto idle little ones before any big core takes a second task.
  - Among busy CPUs, the task goes where it gets the most capacity per task: capacity over tasks, counting itself.
  - With equal capacities this is the old least-loaded rule, so QEMU and the RV2 place tasks as before.
  - The CPU nudged to take a task stuck behind a higher priority is now the fastest idle one, not the first.
- **`V3.4 classes`** checks that every online CPU has a class, the classes add up to the CPUs online, the fastest is 1024, and no cluster mixes types.
- **`V3.4 placement`** makes CPUs 1 and 2 a big and a little core for the test (capacities 1024 and 256), so it runs on any machine with three CPUs. Three tasks that may run on either land on the big core, the little core, then the big core again.
- **`V3.4 costs`**: a Signal and Wait round trip on one CPU and across two, per class (identity layout):

  | Class | One CPU | Across two |
  |---|---|---|
  | Cortex-A720 | 465 ns (884 cycles) | 2.9 µs |
  | Cortex-A520 | 921 ns (1658 cycles) | 3.7 µs |

- **`V3.2 acpi`** builds a PPTT too: a package with an ID of its own, a cluster, one CPU flagged a leaf and one that is only childless. Two more broken sets make 15: a parent past the table's end, and a parent inside a node rather than at its start.
- **Later:**
  - Moving a running task up. A task running alone on a little core stays there when a big core goes idle; only ready tasks are taken.
  - A real policy: which servers go where, and energy (V4).

### The plan, as written before the step

- **What the CIX has:** eight A720s, at four clock grades from 2.2 to 2.6 GHz, and four A520s at 1.8 GHz. The A520 does 0.39 of an A720's work per clock. VQ8 asks for placement by core class.
- **In V3:**
  - each CPU gets a class, from the PPTT and MIDR;
  - the costs are measured per class;
  - placement prefers big cores and spills onto little ones.
- **Later:** a real policy (which servers go where, and energy) waits until V4 has servers to place.

## 5. Arm costs ✅

**Done (2026-10-20).** The profiler runs on AArch64, kernel calls take a lean path like RV64's, and `V3.5 costs` measures the four costs on the first CPU of each core class. On the CIX, in cycles (identity layout):

| | Null kernel call | Domain switch | Signal + Wait | `DoMsg` |
|---|---|---|---|---|
| Cortex-A720 at 1.9 GHz | 258 | 63 | 882 | 1,533 (806 ns) |
| Cortex-A520 at 1.8 GHz | 586 | 66 | 1,666 | 3,366 (1.87 µs) |
| RV2, X60 at 1.6 GHz (V2) | 469 | 118 | 1,554 | 3,209 (2.0 µs) |

Both layouts pass on the board (a soak of 10 boots, 5 in each layout, had no failures). The global layout gives the same numbers to within 1%: `DoMsg` 1,529 on the A720 and 3,380 on the A520. The transcript ends with `V3.5 done`.

- **Per clock, the A520 is a little behind the X60** (null call 586 against 469, `DoMsg` 3,366 against 3,209). Both are in-order cores. The A720 takes about half the X60's cycles for each kernel path, and 0.4 of its time for `DoMsg`. The domain switch is one TTBR0 write with a 16-bit ASID (read from `ID_AA64MMFR0`) on both Arm cores, and needs no flush.
- **`V3.5 costs`** (`selftest_costs`): for each class, everything it measures is pinned to that class's first CPU. That covers `bench` and `pingpong` with the echo server, and processes now take an affinity (`process_create_on`). With an argument, `bench` exits with the null call's cycles and `pingpong` with its round trip's, so the kernel prints the table itself. The clock is the cycle counter against the timer over a millisecond.
- **The kernel-call fast path** (`svc_entry` in `arch/aarch64/vectors.S`). The EL0 sync vector checks ESR's class first. A kernel call saves x0–x5, the call number, x30, SP_EL0, ELR and SPSR, and nothing else. On the way out x1–x18 are zeroed. `callregs` now checks that on AArch64 too: every scratch register comes back zero. Before the fast path it failed all 30,000 calls.
- **What the cycles came from**, on the A720 at `a00`:

  | Change | Null call | `DoMsg` |
  |---|---|---|
  | Before | 314 | 1,632 |
  | The fast path | 302 | 1,614 |
  | CPACR written on the way back to EL0 only when FP access changes, and with no `isb` (the `eret` synchronises) | **258** | **1,533** |

  The profiler found the second: the edge from the end of a call to the return to EL0 was 148 cycles, and is 104 now.
- **The profiler** (`make PROFILE=1`) marks the AArch64 trap path as it does RV64's, and `PMCCFILTR_EL0` is set to count EL0 and EL1 whatever the firmware left.
- **Later:** the parked lock-pair work (docs/v2-plan.md, last section), measured on both ISAs. The A720's lock pair is 27 cycles and the X60's about 55. The RV2 also needs the step 5 build run again. Its numbers above are from V2, because the serial cable is on the CIX.

### The plan, as written before the step

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
2. **One UEFI image, or a CIX platform.** **Decided (2026-10-20): one `uefi-a64` image**, with board differences as quirk data keyed by ACPI IDs. This is the point of ACPI. The RK3588 will still use `booti` and the FDT.
3. **What VQ3's decision does to the losing layout:** keep it behind `layout=` as an option, or delete it. **Decided (2026-10-20): delete it**, after V3, in its own commit, so the decision shows in the history.
