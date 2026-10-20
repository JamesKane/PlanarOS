# V4 plan: the hardware model, drivers and servers

**Status: in progress (2026-10-20).** The four decisions at the end are made, as leaned. Step 1 is under way.

**Goal (§8):** drivers and servers in user mode, found through one device graph, on the RV2 and the CIX P1. The design is in [§9](09-hardware-description.md) (the hwdb, sources, quirks, `acpid`, `power`), [§11](11-driver-model.md) (binding, teardown, `drv.library`, classes, the kill test) and [§12](12-let-it-crash.md) (supervision). This plan puts them in order and says what each step proves.

**What's already built.**
- The kernel parses the device tree and ACPI's static tables (`kernel/hw.c`), and drives only what it needs itself: the console UART, the interrupt controller, the timer, PSCI or SBI, and the watchdogs.
- User programs are flat images built into the kernel (`conformance/`). Only the kernel starts processes.
- The calls are tasks, signals, Areas, ports and connections (23 of them). There are no timers, leases, interrupt bindings, handle passing or `CreateProcess` yet.
- The address space is identity-mapped (VQ3), so an Area's address is its DMA address on a board with no IOMMU in the way.
- The EFI stub already keeps the GOP framebuffer in the boot-information block, and U-Boot leaves one scanning out on the RV2 when a monitor is plugged in (omega's `fbcon.c` draws on it).

**Exit (§8):**
- Each board boots from its own storage to a shell on the framebuffer.
- The same drivers bind through the device tree and through ACPI: on QEMU both ways, the RV2 by its device tree, and the CIX by ACPI.
- Drivers bind in dependency order, taken from the graph, and restart after being killed. The kill test passes (§11.10) on QEMU and the RV2.
- 4 KiB random-read IOPS and latency are measured at queue depths 1, 2, 8 and 32 on NVMe and on SD, with traps and crossings per I/O (§11.8).
- The manuals are current (roadmap rule), with a first *Devices* volume.

Each step ends with `make test-all` green and a `V4.n done` line in the transcript. Steps 1 and 2 run on QEMU and the hosted build only.

| Step | Content |
|---|---|
| **1. The kernel calls drivers need** | A boot bundle of servers, `CreateProcess`, handle passing, timers, device leases, interrupt binding, DMA Areas with quarantine, and the firmware tables as a read-only Area. |
| **2. hwdb on the host** | The device graph from the device tree and from ACPI's static tables, quirks as data, match tables, ordering and the "what blocks whom" report. Tested against the captured trees and the CIX dumps. |
| **3. hwdb as the first server** | hwdb starts drivers in graph order, supervises and restarts them. `drv.library`. The first drivers are small ones on QEMU, and the kill test starts here. |
| **4. `acpid`** | ACPICA in its own domain on QEMU and the CIX, as a second source for the hwdb. Its grants go through the hwdb. |
| **5. Storage** | The `pci` server (ECAM, MSI-X through the ITS), NVMe on QEMU and the CIX, SDHCI on the RV2, the block class, GPT partitions. The queue-depth measurements. |
| **6. Files and programs** | FAT32 under `dos`, programs loaded from storage, and the boot moved onto each board's own storage. |
| **7. A shell on the framebuffer** | The firmware framebuffer leased to `display`, a text console on it, and a shell. Then `power` (the K1 PMU, SCMI on the CIX) and native scanout where the exit needs it. |

## 1. The kernel calls drivers need

§2.2 lists them. V4 adds the ones a driver can't do without, each with conformance programs on both ISAs:

- **A boot bundle.** The servers and drivers built into the image, as `conformance/` programs are now, listed by name. The kernel starts the first one (hwdb), and hwdb starts the rest with `CreateProcess(name, …)`. Programs on storage come in step 6.
- **Handles across processes.** `SendHandle` on a connection, and `CloseHandle`. A port, Area, lease or process handle can be passed. hwdb hands a driver its leases this way.
- **Process control.** `CreateProcess` returns a process handle. A death notice reaches whoever holds it, and `KillProcess` takes it. hwdb needs both to supervise.
- **Time.** A timeout on `Wait`, or `CreateTimer`/`ArmTimer`. Drivers need timeouts, and the restart backoff needs time.
- **Device leases.** `GrantLease` maps a device's MMIO pages into a process, as device memory (`Device-nGnRE` on AArch64, PMA I/O on the K1). Only hwdb may create a lease, from a range it checked: never RAM, the interrupt controller, the IOMMU or the kernel's own devices. That's the check VectraOS-Odin's devmgr made.
- **Interrupts.** `BindIrq(lease, line, signal)` and `AckWaitIrq` (§11.7): an interrupt becomes a signal to the bound task. Edge-triggered sources aren't masked. Death masks the line before anything else (§11.4 step 4).
- **DMA Areas.** `AllocDmaArea(lease, size, flags)`. At exit these Areas go to a quarantine list, and come back only when hwdb says the device has stopped (§11.4). The kernel log and a query report what's held.
- **The firmware tables** as a read-only Area: the DTB, or the RSDP and every table it reaches. Only hwdb gets it.

**Tested by** conformance programs that misuse each call. A lease over RAM, or over the GIC, is refused. A process killed with an interrupt bound and DMA outstanding shows the line masked and the Area quarantined. QEMU devices give a real target: the `edu` PCI device has DMA and interrupts, which is what it was made for.

## 2. hwdb on the host

hwdb's graph logic is pure C (§11.5), so it's built and tested on the `hosted` platform first:
- **The FDT source:** nodes, `compatible`, `reg` through `ranges`, `interrupts` through `interrupt-parent` and `interrupts-extended`, and the references (`clocks`, `resets`, `power-domains`, `*-supply`, `phys`) as `hw_ref` edges.
- **A static ACPI source** for what needs no AML: MCFG, the GIC and its ITS from the MADT, IORT, the SPCR and DBG2. The namespace (`_HID`, `_CRS`, `_DSD`) comes from `acpid` in step 4.
- **Quirks** as C tables (§9.5 D5, proposed (a)), keyed by the root `compatible` or the ACPI OEM IDs.
- **Matching** from each program's match table (§11.6), with a `make check` test for ties.
- **Ordering:** ready when every required supplier is bound, re-examining only the consumers of a node that binds, and a report of each node's state.

**Tested against** the QEMU trees for both ISAs, the RV2's tree from U-Boot, and the CIX's static tables (`docs/boards/cix-p1/acpi`). Each node is expected to get the driver it should, or to say what it waits for.

## 3. hwdb as the first server

- hwdb runs first, builds the graph from the firmware Area, and starts each driver when its node is ready, handing it the node's leases.
- **`drv.library`** owns the driver's main loop: `Bind`, `Start`, `Interrupt`, `Request` and `Stop` (§11.7), death notices, timers, and the deadline.
- **Supervision:** a driver that dies is restarted with backoff, and given up after a few failures in a short window (VQ23). The node keeps its unit.
- **The first drivers are small ones QEMU has:** the PL031 or Goldfish RTC, and the `edu` device. They're there to prove binding, restart and quarantine before a real class depends on them.
- **The kill test starts** with them: kill a driver at random points, over and over, and check its leases, its line, its DMA Areas and its handles each time.

## 4. `acpid`

- **ACPICA, unchanged,** in its own domain (§9.4 D3), with an OS layer in C over Planar's calls. VectraOS-Odin's was about 1,100 lines.
- It enumerates the namespace for the hwdb (`_HID`, `_CID`, `_CRS`, `_STA`, `_DSD`), and on the CIX it keeps running, to evaluate power resources' `_ON` and `_OFF` and `_TMP`.
- **Each region it asks to touch goes through the hwdb**, which refuses RAM, the interrupt controller and the IOMMU (§9.4, R5).
- **On QEMU first** (edk2's DSDT is small), then on the CIX. This step also does §9.6's count: how many of the CIX's devices need a method run at all.
- **The exit's "both ways" is checked here:** on QEMU, the same devices from the device tree and from ACPI, compared as `compare-hw` compares the kernel's facts.

## 5. Storage

- **`pci`:** an hwdb source (§11.6). It takes ECAM from MCFG or the device tree's host bridge node, and publishes a node per function with its BARs and MSI-X. Both boards' firmware trains the links, so V4 drives only generic ECAM. MSI-X needs the GIC's ITS on the CIX and QEMU (deferred from V3).
- **NVMe:** on QEMU (`-device nvme`) and the CIX (its SSD is the board's boot disk). One admin queue, and one I/O queue per CPU class to start.
- **SDHCI on the RV2:** the K1's SD controller, already clocked by U-Boot. eMMC is the same driver if the board has it fitted.
- **The block class** (§11.9): batches of requests per message, a partition server for GPT, and a client library that retries after a restart.
- **The SMMUv3 on the CIX:** FreeBSD ran its NVMe through the SMMU. Planar starts with it in bypass, if the firmware left it so *(verify)*, and the lease records the DMA as unconfined (VQ9).
- **Measured:** 4 KiB random reads at queue depths 1, 2, 8 and 32, on QEMU's NVMe, the CIX's SSD and the RV2's SD card, with traps and crossings per I/O. Lent buffers go to the device as they are, which is VQ3's DMA benefit, measured for the first time.
- **The kill test** on a block driver under I/O, on QEMU and the RV2 (§11.10).

## 6. Files and programs

- **FAT32** as a `dos` handler, read-write, on the EFI partition and on the RV2's SD boot partition (VQ14).
- **Programs from storage.** The shell and its commands live on FAT and are loaded by a loader in user mode (§2.2: no ELF loading in the kernel). Only the boot bundle stays inside the image.
- **Boot from storage.**
  - The CIX already boots `planar.efi` from its EFI partition.
  - The RV2 boots from U-Boot's `load mmc` instead of `loadx` over serial. `tools/serial-boot.py` keeps the serial path for development.

## 7. A shell on the framebuffer

- **`display`** leases the firmware's framebuffer: the GOP's on the CIX and on QEMU through edk2 (`-device ramfb`), and U-Boot's on the RV2. A text console draws on it, with omega's 8×8 font to start.
- **A shell** in the Amiga style: commands from FAT, run as processes, with their output on the console.
- **`power`** with the providers these boards need: the K1 PMU on the RV2 and native SCMI on the CIX (§9.4 Layer 3). Its first job is the kill test's check that a dead driver's votes go.
- **Native scanout** (the K1 DPU, the CIX's Linlon) moves to V5, next to the GPU it composes with (decision 2).

## Carried in from V3

- **Identity's needless failures under pressure** (VQ3): about 0.15% of allocations fail with enough memory free. Drivers' DMA pools are the first real large allocations, so V4 tries reserving long runs for large Areas, measured with `make vq3`.
- **The ITS and MSIs, and the SMMUs**, deferred from V3 to V4 with PCIe.
- **The GOP framebuffer as a console**, deferred from V3 (step 7).

## Decisions

1. **Input for the shell.** The exit says "a shell on the framebuffer" but doesn't say where keystrokes come from. The options are:
   - (a) the serial console, with output on both;
   - (b) a USB keyboard, which means an xHCI driver (DWC3 on the RV2, the CIX's controllers) and USB HID first.

   **Decided (2026-10-20): (a)** for the V4 exit, and USB in V5 alongside `input`.
2. **Scanout.** Is the firmware's framebuffer enough for the exit, or does V4 need native DPU drivers (the roadmap names the K1 DPU and Linlon)? **Decided (2026-10-20): the firmware framebuffer** for the exit, with native scanout in V5, where the GPU and compositing need it.
3. **Program format.** Keep flat images for programs on storage, or load ELF? **Decided (2026-10-20): ELF**, through a loader in user mode, since programs on storage are no longer built with the kernel.
4. **Where the boot bundle comes from.** Built into the image, as the conformance programs are now, or a separate file the firmware loads next to it? **Decided (2026-10-20): built into the image** for V4. A separate file comes when programs outgrow it.
