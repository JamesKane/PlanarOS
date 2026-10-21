# 9. Hardware description: ACPI and FDT (VQ7 design session)

**Status: decided on 2026-10-19** (D2, D3, D4, D6 answered in the session; D1 follows from D2; D5 proposed). §9.6 lists the remaining items.

## 9.1 Problem

Boards from the last year ship ACPI (the Q8B, the Orange Pi 6 Plus with the CIX P1, and the SpacemiT K3). The older boards ship FDT (the RV2 and the RK3588). Planar has to run on both, and nobody wants every driver written twice.

## 9.2 What the firmware actually provides

**Q8B**
- **ACPI:** tables FACP (hardware-reduced), APIC, GTDT, IORT, PPTT, MCFG, SPCR, DBG2, CSRT, TPM2. The DSDT is 132 KB, with 163 devices and 514 methods.
- **What the AML does:** very little Planar could use. The 14 power resources are empty stubs. 103 devices `_DEP` on Windows PEP (`QCOM0617`), which is a pile of static clock, regulator and bus packages that only the Windows PEP driver understands. There are no `_CPC`, `_PSS` or `_TMP` methods, and the 32 thermal zones have no `_TMP`.
- **Firmware DTB:** also provided, and AbyssBSD took its PEP-hidden addresses from it.
- **Quirks:**
  - SPCR has a bad access width and the wrong IRQ.
  - GPIO consumers describe the Qualcomm reference board (QRD08280), not this one, and the `_AEI` is bogus.
  - The RTC (an M41T11 on I²C) isn't described, and the ACPI Time and Alarm device is disabled.
  - Some IRQs exist only in the DT.
  - Two GPU resources overlap.
- Source: `AbyssBSD/docs/boards/radxa-dragon-q8b/dumps/acpi/`.

**CIX P1**
- **ACPI:** only ACPI. Tables APIC, GTDT, IORT, MCFG, PCCT, PPTT, SDEI, DBG2, CSRT, plus the DSDT and 2 SSDTs. No SPCR.
- **What the AML does:** real work, and the GPU, NPU and VPU need it:
  - Power-resource `_ON` methods that poke registers and run memory repair.
  - `_CPC` (an SCMI fastchannel and AMU counters) and `_LPI`.
  - 13 thermal zones with `_TMP`.
  - `_DSD` properties in DT style (`mboxes`, `shmem`, `power-domains`).
- **SCMI in AML is broken:** the AML's SCMI clock agent returns NOT_FOUND, so AbyssBSD spoke SCMI natively.
- **Quirks:** the RSVL carve-outs are reported as free memory. Some objects are missing (`I2C0.UXC*`).
- **Dumped on 2026-10-20:** [boards/cix-p1](boards/cix-p1/README.md) has every table.
- Source: `AbyssBSD/docs/boards/orangepi-6-plus/`.

**K3**
- ACPI on RISC-V means RHCT, plus RINTC, IMSIC and APLIC entries in the MADT.
- No local notes or dumps.

**RV2 and RK3588**
- **FDT:** U-Boot passes it. Mainline device trees are good.
- **Quirks:** on the RV2 the timebase in the vendor DT is wrong.

AbyssBSD learned three things on these boards that apply here:
1. Firmware tables describe a reference design or what Windows needs, so check everything a driver reads from them.
2. Linux on the same board is the ground truth.
3. A per-board setting is a sign of a driver bug.

## 9.3 Requirements

- **R1.** A driver is written once, against one model. It never parses ACPI or FDT itself.
- **R2.** The kernel stays small (VQ12). It parses only what it needs before any server runs.
- **R3.** Broken firmware gets fixed in data, per board, without touching drivers.
- **R4.** Power, clocks and resets work the same way whatever describes them: SCMI, RPMh, RK806/CRU, or ACPI power resources.
- **R5.** Running AML must not compromise the rest of the system. In ACPI, AML is arbitrary code that can touch arbitrary MMIO.
- **R6.** When an ACPI board's tables are hopeless, a DTB can be used instead.

## 9.4 Proposed design

```
            firmware                     kernel (early)                user mode
 ┌───────────────────────┐   ┌───────────────────────────────┐   ┌─────────────────────────────┐
 │ FDT blob              │──▶│ fdt.c  ─┐                     │   │ hwdb server                 │
 │ ACPI RSDP/XSDT tables │──▶│ acpi.c ─┴─▶ struct bootinfo   │   │  ├ fdt source               │
 │ (firmware DTB on Q8B) │   │   memory map, reserved ranges │   │  ├ acpi source ◀── acpid ───┼─ ACPICA, AML
 └───────────────────────┘   │   CPUs + topology, IRQ ctlr,  │   │  ├ board quirks (data)      │
                             │   timer, console              │   │  └▶ device graph            │
                             │ tables → read-only Area ──────┼──▶│                             │
                             └───────────────────────────────┘   └──────────────┬──────────────┘
                                                                                │ match, props, resources
                                                            drivers ◀───────────┘   power server ◀── providers:
                                                                                     SCMI · RPMh · RK806/CRU · acpi-pr
```

### Layer 0: the kernel's early parsers (D1)

Two small parsers fill one `struct bootinfo`. The kernel uses nothing else.

| Need | FDT | ACPI |
|---|---|---|
| Memory map, reserved ranges | `/memory`, `/reserved-memory`, memreserve | EFI memory map (from the stub), plus quirks for broken reservations such as CIX's RSVL |
| CPUs, IDs, topology, core class | `/cpus`, `cpu-map`, `capacity-dmips-mhz` | MADT (GICC / RINTC), PPTT |
| Interrupt controller | `interrupt-controller` nodes | MADT (GICD/GICR/ITS; PLIC/APLIC/IMSIC on RISC-V) |
| Timer | `/timer`, `timebase-frequency` | GTDT; RHCT on RISC-V |
| Console | `stdout-path` | SPCR, then DBG2, then a quirk (CIX has no SPCR; the Q8B's is wrong) |
| PCIe ECAM | Not needed in the kernel | Not needed in the kernel; MCFG goes to the hwdb |

- Omega's `fdt.c` (384 lines) carries over. It gains phandle, `interrupts` and `ranges` helpers.
- `acpi.c` walks static tables only and never runs AML. Its estimated size is 500–800 lines; the Rust port's `acpi.rs` is 470.
- **Built (V3 step 2), differently from the diagram:** there is no `struct bootinfo` of facts.
  - `kernel/hw.c` chooses one source (R6: a DTB wins, unless `hw=acpi`) and forgets the other.
  - Each consumer asks the chosen parser directly. `acpi.c` is 490 lines.
  - Every boot prints the facts it took as `hw:` lines, and `make compare-hw` checks that QEMU gives the same lines from both sources.
  - The early quirk table lives in `acpi.c`: the CIX's DBG2 console and RSVL ranges.
- **Built (V3 step 4):** the PPTT's clusters, read per CPU (`acpi_cluster`). Speeds aren't read from the firmware: the kernel measures them (`kernel/cpuclass.c`).
- **Built (V3 step 3):** on the CIX P1, every fact the kernel uses comes from ACPI and the quirk entry. The GTDT's watchdog is driven (`drivers/sbsa_wdt.c`).
- The raw blobs are then handed to user space as a read-only Area.

### Layer 1: hwdb, the device graph (D2)

A user-mode `hwdb` server builds a single device graph from one or more **sources**. Linux's `fwnode` and unified device-property API are the precedent: the same calls read DT properties and ACPI `_DSD` properties.

```c
struct hw_node {
    hw_id      id, parent;
    hw_keys    match;        /* "rockchip,rk3588-mali", "arm,mali-valhall-csf", "CIXH5010", PNP _CID … */
    hw_res     res[];        /* MMIO (base,len,attr) · IRQ (ctlr,line,flags) · DMA (iommu,stream-id) · GPIO · I2C/SPI addr */
    hw_ref     refs[];       /* power-domain, clock, reset, regulator, phy → (provider node, specifier cells) */
    hw_props   props;        /* typed key/value: DT properties or _DSD device-properties */
    hw_status  status;       /* DT "status" / ACPI _STA */
};
```

- Drivers find nodes by matching keys. A driver lists every key it supports, both DT compatibles and ACPI HIDs. The Panthor server, for example, lists `arm,mali-valhall-csf` and the CIX GPU HID.
- Drivers read resources and properties through `hw.library`. They never see where the data came from (R1).
- References are first class. `power-domains = <&scmi 8>` in a DT and `power-domains = <\_SB.SCMI.DVFS 8>` in CIX `_DSD` become the same `hw_ref`.
- The hwdb hands out leases. Each resource in the graph is what `GrantLease` (§2.2) gives to the driver that binds the node.
- **Built (V4 step 2), on the host:** `servers/hwdb/hwdb.h` has `struct hw_node` much as above, with the state and the consumers added. `props` aren't copied: a node keeps where its properties are in the blob. Static ACPI nodes take the device tree's compatible (`arm,smmu-v3`), since the tables carry no `_HID`. Quirks are C tables (D5 (a)), with four actions so far: disable, the firmware's, the console, and pin or exclude a driver. Leases are whole pages, and the report lists the pages devices share: QEMU's virtio-mmio slots, and the K1's system controllers ([v4-plan](v4-plan.md) step 2).
- **Built (V4 step 3):** leases come from pages (`servers/hwdb/lease.c`). Devices sharing a page are one lease, served by one process of their driver, as QEMU's virtio slots are. A page shared across drivers, or with a device the kernel or the firmware drives, can't be leased. On the RV2 that's the K1's power domains: their registers share the MPMU's pages with the clock controller, which U-Boot set up, and the run of pages takes in the kernel's watchdog too. `power` will need a quirk, or the clock controller and the kernel's watchdog moved under it (step 7).

### Layer 2: sources and quirks (D3, D4)

- **FDT source:** a direct translation.
- **ACPI source:** `acpid` enumerates the namespace (`_HID`, `_CID`, `_CRS`, `_STA`, `_DSD`) and gives the results to the hwdb.
  - **Built (V4 step 4):** `servers/acpid/` runs ACPICA 20260930, unchanged (`third_party/acpica`), and sends hwdb a record per device (`planar/acpid.h`). hwdb adds them before it matches anything (`servers/hwdb/namespace.c`). A device a static table already has moves the static node to its namespace path (`/_SB_/COM0`) rather than appearing twice. On QEMU the same drivers bind through ACPI as through the device tree, and `make compare-hw` checks it. `_DSD` is read only for `PRP0001`'s "compatible" so far.
  - On CIX it also *keeps running* as a provider: power-resource `_ON`/`_OFF` for the `power` server, `_TMP` for thermal, and anything else that only exists as a method.
  - `_CPC` and `_LPI` are read once and stored as static data.
- **Merging sources (Q8B):** ACPI is the primary source. The firmware DTB is a second source, used only for the nodes that PEP hides: clock controllers, RPMh and TSENS. They are joined by quirk-table mappings from node to node. This is what AbyssBSD did by hand.
- **Board quirks** are *data*. They are keyed by the DT root `compatible`, or by the ACPI OEM ID / OEM table ID / DMI. Each one can:
  - replace or add a resource (the Q8B SPCR IRQ, a missing SD power IRQ);
  - add a node (the Q8B RTC on I2C);
  - mark memory reserved (CIX RSVL);
  - disable a node (the bogus `_AEI`);
  - map a node from one source onto a node from another.

  Quirks that the kernel's early boot needs (console, reserved memory) are compiled into the kernel as a small table, and the hwdb applies the rest.
- **DTB override (R6):** the boot loader or EFI stub can load a DTB from the EFI system partition. When it does, that DTB replaces ACPI completely for that boot.

### Layer 3: power and providers

- The `power` server owns power, clocks, resets and regulators. A driver asks it to "power my node on". The `power` server follows the node's `hw_ref`s to the providers:
  - **SCMI:** native, for CIX GPU and NPU domains and DVFS fastchannels.
  - **RPMh / cmd-db / GCC:** native, Q8B.
  - **RK806 / CRU / PMU:** native, RK3588.
  - **K1 PMU:** native, RV2.
  - **`acpi-pr`:** acpid executes ACPI power-resource methods (CIX).
- Order of dependencies is the provider's job. For example, CIX's five-step GPU power-on sequence (§5.3) becomes one `PowerOn(gpu)` call that runs SCMI power, SCMI clock, `acpi-pr _ON`, reset and Q-channel in sequence.

### Isolation of AML (R5)

- acpid runs in its own domain. It holds only the MMIO, I/O and PCI-config windows its operation regions ask for. Each grant goes through the hwdb, which refuses RAM, the interrupt controller and the IOMMU, as VectraOS-Odin's devmgr did.
- A misbehaving method can only wreck the device it belongs to.
- **Built (V4 step 4):** memory operation regions go through acpid's own handler, and each window is asked of hwdb once, by pages (`servers/hwdb/acpid.c`). hwdb refuses the interrupt controller, an IOMMU, what the kernel drives and what a driver's lease holds; the kernel refuses memory again. A refused read gives all ones and a refused write is dropped. I/O ports and PCI configuration space are refused outright for now. In test mode acpid probes the policy: the GIC, its own stack and each device's registers. hwdb checks what it leased afterwards.

## 9.5 Decision points

| # | Question | Options | Outcome |
|---|---|---|---|
| **D1** | What the kernel parses | (a) FDT plus static ACPI tables in the kernel, as above; (b) nothing, with a user-mode boot server handing the kernel its facts | **Decided (a).** Follows from D2. The kernel needs the interrupt controller, timer, CPUs and memory before any server exists. |
| **D2** | Where the device graph lives | (a) a `hwdb` server; (b) inside the kernel; (c) each driver parses its own source | **Decided (a): a `hwdb` server.** |
| **D3** | How AML runs | (a) **ACPICA, unchanged, in the user-mode `acpid`**. It has run in user space before (VectraOS-Odin), is BSD-licensed, and its OS layer gets rewritten in C (Odin's was about 1.1k lines). (b) Write a small AML interpreter. (c) No AML: board quirk tables replace every method. | **Decided (a): ACPICA in user-mode `acpid`.** |
| **D4** | Q8B source | (a) ACPI primary plus the firmware DTB for PEP-hidden nodes; (b) DTB only, with ACPI ignored; (c) ACPI only, with hard-coded SoC tables | **Decided (a): ACPI plus the firmware DTB merged.** |
| **D5** | Format of the quirk data | (a) C tables compiled into the hwdb; (b) DT overlays (`.dtbo`) applied to the graph; (c) a small text format | Proposed: (a) to start, with match keys and actions kept stable so the format can move to (b) later. |
| **D6** | Do ACPI boards ship as first-class v1 targets? | Affects VQ19 board order | **Decided: yes. CIX (Orange Pi 6 Plus) is the second board, right after the RV2.** |

## 9.6 Open items for the session

- **The CIX tables are dumped** ([boards/cix-p1](boards/cix-p1/README.md)). **`_CPC` is static** (V3 step 4): in SSDT2 each processor device has a `Name(_CPC, Package)` whose performance levels are integer constants, so acpid can read it once without running a method. **Counted (V4 step 4, [acpid.txt](boards/cix-p1/acpid.txt)):** of 271 devices, 121 take a method to describe, 118 of them by `_STA` and 19 by `_CRS`, and 17 are powered by method. Describing 77 of them reads a firmware variable block at `0xfffe0000` (GNVA, through the `_STA` methods' `GETV`), memory the firmware keeps, which acpid may not read yet (VQ42). **`_LPI` is a method**, in each of the 12 processors, but it only returns `\_SB.LPIB`, a static package: acpid can evaluate it once.
- **K3:** revive the board, then dump RHCT and MADT and confirm it uses AIA.
- **SCI and ACPI events.** Hardware-reduced ACPI uses GED or GPIO-signalled events. Does any target need them in v1? (Power button, lid.)
- **Who owns PCIe enumeration:** the hwdb (ECAM from MCFG, or DT `ranges`) or a separate `pci` server. VectraOS-Odin ran devmgr's PCI scan before ACPI. *Proposed in [§11.6](11-driver-model.md#116-binding):* a `pci` server that is an hwdb source, as `acpid` is, publishing a child node per function.
- **Should `hw_node` and the property API mirror Linux's `fwnode` names** to make driver ports from Linux easier?
