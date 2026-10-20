# V4 plan: the hardware model, drivers and servers

**Status: in progress (2026-10-20).** Steps 1 to 3 are done. The four decisions at the end are made, as leaned.

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
| **1. The kernel calls drivers need** ✅ | A boot bundle of servers, `CreateProcess`, handle passing, timers, device leases, interrupt binding, DMA Areas with quarantine, and the firmware tables as a read-only Area. |
| **2. hwdb on the host** ✅ | The device graph from the device tree and from ACPI's static tables, quirks as data, match tables, ordering and the "what blocks whom" report. Tested against the captured trees and the CIX dumps. |
| **3. hwdb as the first server** ✅ | hwdb starts drivers in graph order, supervises and restarts them. `drv.library`. The first drivers are small ones on QEMU, and the kill test starts here. |
| **4. `acpid`** | ACPICA in its own domain on QEMU and the CIX, as a second source for the hwdb. Its grants go through the hwdb. |
| **5. Storage** | The `pci` server (ECAM, MSI-X through the ITS), NVMe on QEMU and the CIX, SDHCI on the RV2, the block class, GPT partitions. The queue-depth measurements. |
| **6. Files and programs** | FAT32 under `dos`, programs loaded from storage, and the boot moved onto each board's own storage. |
| **7. A shell on the framebuffer** | The firmware framebuffer leased to `display`, a text console on it, and a shell. Then `power` (the K1 PMU, SCMI on the CIX) and native scanout where the exit needs it. |

## 1. The kernel calls drivers need ✅

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

**Done (2026-10-20).** Sixteen new kernel calls (39 in all), on both ISAs. `make test-all` passes its 10 boots at `V4.1 done`, and so does the RV2. The calls are in [Autodocs](../manuals/autodocs/lease.md) and in `planar/exec.h` and `planar/lease.h`.

- **Processes:** `CreateProcess`, optionally held for `StartProcess`, `KillProcess`, `QueryProcess` and `CloseHandle`. A process made by another is watched through a handle its creator's process owns, and never outlives it: closing the handle, or the owner dying, kills it. hwdb's children therefore die with hwdb.
- **Time:** `CurrentTime` (nanoseconds) and `ArmTimer`, one alarm per task. A 2 ms alarm comes 450–600 µs late on QEMU's TCG.
- **Leases** (`kernel/lease.c`):
  - The kernel gives one process a *device root* handle (`process_create_root`). With it, `CreateLease` makes a lease: MMIO pages and up to four lines.
  - A lease is refused over RAM or reserved memory, over the registers the kernel drives itself (now registered in `kernel/devmem.c`: the console, the GIC or PLIC, the watchdogs), over another lease, and over lines the kernel uses.
  - Its owner grants it to one process with `GrantLease`. That process maps the registers at their physical address (Device-nGnRE on AArch64), binds lines to signals, and allocates DMA Areas against it.
  - When the grantee dies, its lines are masked at once, and DMA Areas it hadn't freed are quarantined. The lease is *stopping* until its owner, which may now map the registers to stop the device, calls `RevokeLease`.
- **The firmware's description:** `MapFirmware` maps the DTB, or the RSDP, the XSDT, every table and the DSDT and FACS, read-only at their physical addresses, for the device root's holder.
- **Tested by** `conformance/spawner`, `timer` and `leaser`. `leaser` plays hwdb, and drives `virtiorng`: a user-mode driver for QEMU's virtio entropy device (`-device virtio-rng-device`, on both machines). The driver maps its lease, sets up a virtqueue in DMA Areas, takes the interrupt and checks the bytes. A second driver is killed with its request at the device: its two pages wait in quarantine until `leaser` has reset the device and revoked the lease. Mutations that skip the quarantine, skip the memory check, or skip killing an owner's children each fail the tests.

**Not as planned:**
- **No `SendHandle`.** hwdb keeps its leases and grants them, so a driver never owns one, and a supervisor that owns its leases is the simpler model. Handle passing can come when something needs it.
- **No boot bundle yet.** The programs are still the `conformance/` images. The kernel starting hwdb belongs with step 3.
- **Every line is masked when it fires,** edge-triggered ones too, and unmasked by `AckWaitIrq`. Not masking edge lines is an optimisation to measure later.
- **The IOMMU isn't refused yet:** the kernel doesn't know where the SMMUs are until it reads the IORT (step 5).
- **The firmware tables aren't an Area:** they're mapped where they lie, because they live in firmware memory and not in pages the kernel allocated.
- **The test device is virtio, not `edu`:** `edu` is on PCI, which comes in step 5; virtio-mmio is in both QEMU device trees now. Under ACPI, and on the boards, the lease test runs its refusals and the firmware check only.

**Fixed on the way:**
- `ktimer_stop` and `irq_detach` now wait for a handler already running on another CPU, so what the handler uses can be freed once they return.
- The process record holds a reference of its own while the process lives, and the watcher's fields are read before `dead` is set.

## 2. hwdb on the host ✅

hwdb's graph logic is pure C (§11.5), so it's built and tested on the `hosted` platform first:
- **The FDT source:** nodes, `compatible`, `reg` through `ranges`, `interrupts` through `interrupt-parent` and `interrupts-extended`, and the references (`clocks`, `resets`, `power-domains`, `*-supply`, `phys`) as `hw_ref` edges.
- **A static ACPI source** for what needs no AML: MCFG, the GIC and its ITS from the MADT, IORT, the SPCR and DBG2. The namespace (`_HID`, `_CRS`, `_DSD`) comes from `acpid` in step 4.
- **Quirks** as C tables (§9.5 D5, proposed (a)), keyed by the root `compatible` or the ACPI OEM IDs.
- **Matching** from each program's match table (§11.6), with a `make check` test for ties.
- **Ordering:** ready when every required supplier is bound, re-examining only the consumers of a node that binds, and a report of each node's state.

**Tested against** the QEMU trees for both ISAs, the RV2's tree from U-Boot, and the CIX's static tables (`docs/boards/cix-p1/acpi`). Each node is expected to get the driver it should, or to say what it waits for.

**Done (2026-10-20).** `servers/hwdb/`, about 2,400 lines of C with no library, which compiles freestanding for both ISAs (only `memset` from outside), so step 3 can run it as the server unchanged. `make hwdb-test` (part of `make test-all`) ends with `V4.2 done`; step 2 runs on the host only, so that line is the test's, not a boot's.

- **The graph** (`hwdb.h`, `graph.c`): nodes with keys, resources (MMIO, interrupts with their line, bus addresses, ECAM), references (`hw_ref`: parent, interrupts, clock, reset, power domain, supply, phy, pins, GPIO, and the optional IOMMU, MSI, DMA and nvmem), and a state. All memory comes from one arena.
- **The device tree source** (`fdt.c`): `reg` translated through each bus's `ranges` (a bus without them, as I2C or `/cpus`, gives bus addresses), interrupts through `interrupt-parent`, `interrupts-extended` and `interrupt-map`, PCI's `msi-map` and `iommu-map`, and the references above. A reference's supplier is the device its target belongs to, so a pin group means its pin controller and a PMIC's regulator means the PMIC. The console is `stdout-path`'s node.
- **The static ACPI source** (`acpi.c`): the MADT's GIC (with its redistributors) and ITSs, a host bridge per MCFG range, the IORT's SMMUv3s and named components, with its ID mappings as references (a host bridge to its SMMU, and through it to the ITS for MSIs), the SPCR's and DBG2's serial ports, and the GTDT's timer and watchdogs. Static tables carry no `_HID`, so these nodes take the device tree's compatible for what they are (`arm,gic-v3-its`, `arm,smmu-v3`), and host bridges take `PNP0A08`: one match table then binds both ways. A named component gets no key: acpid gives it one (step 4).
- **Quirks** (`quirks.c`): C tables keyed by the root compatible or the OEM IDs, naming nodes by path, to disable a node, mark it the firmware's, name the console, or pin or exclude a driver. Two boards have entries: the CIX's console is DBG2's port (as the kernel's quirk says), and on the RV2 the K1's clock, reset and pin controllers are U-Boot's until `power` takes them (step 7).
- **Matching** (`match.c`, `drivers.c`): the node's most specific key that any driver takes decides, and the rank settles which driver takes it. The tables are in `drivers.c` until drivers are ELF (step 6): the kernel's own devices, M-mode's, hwdb's (`simple-bus`, fixed clocks), and the drivers V4 plans, none of which is a program yet. `make check` fails on a tie.
- **Ordering** (`order.c`): a node is ready when its references are met; when one binds, only its consumers are looked at again. An optional reference is dropped at the start if its supplier can't bind, and, when nothing is ready, if it's what holds a node back, which breaks a cycle through it.
- **The report** (`report.c`): each device with its driver and when it bound, or what it waits for and that supplier's state; with `--detail`, its resources and references. Then the pages devices share (see below).
- **Tested** two ways. `servers/hwdb/test.c` builds graphs that break each rule (cycles with and without an optional edge, a phandle naming nothing, a disabled supplier, the count of nodes looked at again, ranks, ties, each quirk), then reads every blob and table cut short, changed at random, and shortened with its length and checksum mended, under the address sanitizer. Removing either of two bounds checks makes it fail. Then each captured description must give its report exactly: `docs/boards/qemu-virt/{rv64,a64}-hwdb.txt`, `docs/boards/orangepi-rv2/hwdb.txt` and `docs/boards/cix-p1/hwdb.txt`.
- **The captures.** QEMU's trees, and the RV2's as U-Boot passes it, were taken by the kernel itself: the boot argument `dumpdtb` prints the blob as hex (`fdt_dump_blob`), and `tools/dtb-from-log.py` rebuilds it from the transcript. They include what OpenSBI and U-Boot change, which `dumpdtb` from QEMU or the dts in Linux's tree wouldn't. The RV2's is U-Boot's own control tree, `binman` nodes and all.

**What the reports say:**
- **QEMU:** everything planned binds, in order (the PL031 after its fixed clock). The ITS has no driver, so PCI's MSI reference is dropped.
- **The RV2:** the eMMC's controller (`sdh@d4281000`) binds. The SD card's (`sdh@d4280000`) waits for the K1's GPIO controller, which reads its card detect: step 5 needs a GPIO driver, or a card that's assumed present.
- **The CIX:** the three host bridges bind; their SMMU (`0xb010000`) and the ITS have no driver, so both references are dropped (the SMMU in bypass, as step 5 plans). The fourteen named components (DPUs, the NPU, the ISP) wait for acpid.

**Found: devices that share pages.** A lease is whole pages and no page is in two, but:
- QEMU AArch64's 32 virtio-mmio slots are 0x200 apart, eight to a page (the step 1 test leased one page for one device).
- On the K1, the clock controller, the reset controller, the power domains and the kernel's watchdog share the MPMU's pages at `0xd4050000`, and the first three share `0xd4282000` with the PCIe controller and its PHY. `CreateLease` would refuse `power` the first: the kernel uses it.

Step 3 has to choose: one lease per run of shared pages, held by one driver that serves the others (a virtio-mmio bus driver for its slots; `power` for the K1's system controllers, with the watchdog's register reached through it or kept as the kernel's), or leases smaller than a page. The report lists each such run. **Chosen (2026-10-20): one lease per run**, built in step 3 (`servers/hwdb/lease.c`).

**Not as planned:**
- **No "what blocks whom" chain,** only each node's direct blockers and their states. Following them takes one look per step, and the real trees' chains are one or two long.
- **Shutdown and resume order** aren't computed yet: the graph has what they need, and they come with `power`.

## 3. hwdb as the first server

- hwdb runs first, builds the graph from the firmware Area, and starts each driver when its node is ready, handing it the node's leases.
- **`drv.library`** owns the driver's main loop: `Bind`, `Start`, `Interrupt`, `Request` and `Stop` (§11.7), death notices, timers, and the deadline.
- **Supervision:** a driver that dies is restarted with backoff, and given up after a few failures in a short window (VQ23). The node keeps its unit.
- **The first drivers are small ones QEMU has:** the PL031 or Goldfish RTC, and the `edu` device. They're there to prove binding, restart and quarantine before a real class depends on them.
- **The kill test starts** with them: kill a driver at random points, over and over, and check its leases, its line, its DMA Areas and its handles each time.

**Done (2026-10-20).** hwdb runs as a server, starts drivers in graph order, and restarts them when they die. `make test-all` passes its 10 boots at `V4.3 done`.

- **The boot bundle** now holds servers and drivers as well as the conformance programs (`mk/user.mk`): `servers/NAME/` and `drv/NAME/` are a program each, every `.c` in the directory, and drivers link `drv.library`. User programs are now position-independent and relocate themselves before `main` (`lib/rt/reloc.c`), since hwdb's match and quirk tables are pointers.
- **Leases come from pages** (`servers/hwdb/lease.c`). Devices whose registers share a page are one lease, served by one process of their driver. A page shared with another driver's device, or with one the kernel or the firmware drives, can't be leased, and those nodes fail with the reason. A device with registers in two places joins them into one lease with two ranges. The report lists each lease that serves more than one device.
- **hwdb** (`servers/hwdb/server.c`). The kernel starts it with the device root. It maps the firmware's description, builds the graph, and starts each lease's driver when all the lease's devices are ready: the process held, its leases granted, then started. It answers the driver's two messages, `HWDB_BIND` and `HWDB_STARTED` (`planar/drv.h`), and never waits on a driver. A driver that takes none of its devices exits, and they're *declined*.
- **Supervision.** When a driver dies, hwdb checks what the kernel did: the leases are stopping, no DMA Area is left live, and, when hwdb killed it while running, every page it held is in quarantine. Then it runs the stop method, revokes the leases (which frees the quarantine), and starts it again. A death hwdb didn't ask for is a failure: restarted after 10 ms, then twice as long each time, and given up on at 5 failures in 10 s. The devices then fail, and their consumers wait. A driver that hasn't started in 2 s is killed as failed.
- **Stop methods.** virtio's status register is the only one so far. A device with none can be restarted only if it held no DMA. Otherwise its quarantine stays, as VQ24 leans, and it isn't restarted.
- **`drv.library`** (`lib/drv`): `Bind` for each device, then `Start` to publish its class port (`rtc.0`, `rng.1`), then a loop that hands interrupts to `Interrupt` and requests to `Request`. It maps the leases and binds each taken device's line. Line *n* of a lease is signal 32 + *n*.
- **Three drivers** (`drv/`): `rtc-pl031` (QEMU AArch64), `rtc-goldfish` (QEMU RISC-V), and `virtio-mmio`, which serves a page of slots and drives the entropy device in them.
- **The kill test** (`servers/hwdb/killtest.c`, hwdb's test mode, run by `kernel/selftest.c` as V4.3):
  - A client task for each class port reads its device over and over, and reconnects after `ERR_PORT_DIED`.
  - hwdb kills each live driver 12 times at random points, about a third of the kills while it's still binding, and checks the teardown above each time. Its own handle count mustn't grow.
  - Then one driver faults in `Bind` on every start. It must be given up on after 5 failures, spaced by the backoff, then start again when retried.
  - The clients must have recovered from every death they saw. Then hwdb kills everything, closes its leases and exits 0. The kernel checks that no process, lease, Area or page outlived it.
  - **Mutations:** a kernel that frees a dead driver's DMA Areas instead of quarantining them fails the "every page in quarantine" check, once per kill of the entropy driver. One that leaves a dead driver's line bound fails the restart: the new driver's `BindIrq` is refused.
- **Kernel changes:** `LEASE_IRQS` went from 4 to 32 (eight virtio slots to a page, each with a line). `QueryProcess(0, …)` asks about the calling process, and `pi_Handles` reports a process's handle count.

**What the boots show:**
- **QEMU RISC-V:** eight virtio slots in separate pages, so eight leases. The entropy device's driver binds, and the seven empty slots are declined. The Goldfish RTC binds.
- **QEMU AArch64:** four leases of eight slots each. One process drives the entropy device, and the other three decline. The PL031 binds after its fixed clock. Through edk2, the PL031 is disabled because the firmware keeps it, so the kill test has the virtio driver alone.
- **Under ACPI, and on the boards,** the only drivers matched (`pci`, `sdhci-k1`, `power-k1`) aren't built, so those nodes fail with "its program isn't built yet", and the kill test has nothing to kill.
- **About 20 ms per kill-and-restart** under QEMU's TCG, and the give-up test takes about 165 ms.

**Not as planned:**
- **No `edu` device:** it's on PCI, which comes in step 5. virtio's entropy device has DMA and an interrupt, so it serves instead.
- **hwdb runs as the kernel's V4.3 test,** in test mode, and exits when the test ends. Starting it at every boot as the first server, and leaving it running, comes when something follows it (step 7's shell).
- **No graceful `STOP`.** Unbind is a kill, the path §11.4 says must be right. A `STOP` with a deadline can come before it when a class needs it (block, in step 5).
- **The supervision is one-for-one.** A restarted supplier's consumers aren't restarted with it (§12.7's rest-for-one). None of these drivers has a consumer yet.
- **No port continuity:** a restarting driver's port name is gone until the new one publishes it (§12.6). Clients retry on `ERR_NOTFOUND`.
- **Tokens, not identities.** hwdb knows a driver by the token in its argument (the lease and a generation), not by which process connected. That's enough for programs from the boot bundle.
- **`drv.library` isn't in the manuals yet.** It goes in the *Devices* volume at the V4 exit, when its interface has met a real class.

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
