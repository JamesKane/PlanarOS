# 2. Architecture overview

## 2.1 Layers

```
 ┌──────────────────────────────────────────────────────────────────┐
 │ Applications           (own their GPU/NPU queue leases directly) │
 ├──────────────────────────────────────────────────────────────────┤
 │ Libraries   exec.library · dos.library · gfx · queue · alloc     │  user mode, shared RX
 │             (run in the caller's domain; published via ExecBase) │  region, global-mapped
 ├──────────────────────────────────────────────────────────────────┤
 │ Servers     dos/fs · display · input · net · audio · power/clock │  user mode, one
 │ Drivers     uart · sd/emmc · nvme · pcie · usb · gpu · npu · dpu │  domain each
 ├──────────────────────────────────────────────────────────────────┤
 │ Kernel      domains · tasks · scheduler · ports/rings · signals  │  S-mode / EL1
 │             handles · timers · IRQ routing · leases · KLog       │  ~40 calls
 ├──────────────────────────────────────────────────────────────────┤
 │ arch/       rv64 · aarch64 · (hellfire)    plat/  ky-x1 · rk3588 │
 │             · cix-p1 · sc8280xp · qemu-virt · hosted             │
 ├──────────────────────────────────────────────────────────────────┤
 │ Firmware    OpenSBI · TF-A · UEFI · Qualcomm hyp/TZ (not ours)   │  M-mode / EL2 / EL3
 └──────────────────────────────────────────────────────────────────┘
```

## 2.2 Kernel responsibilities

**Proposed.** This follows Omega's ~40-call kernel. The target is 10–15k lines of C, excluding `arch/` and `plat/`.

| Group | Calls (indicative) | Notes |
|---|---|---|
| Domains and memory | `AllocArea`, `ShareArea`, `UnshareArea`, `ProtectArea`, `FreeArea`, `QueryArea` | The Area is the only memory abstraction the kernel has (§3). |
| Tasks and processes | `CreateProcess`, `CreateTask`, `ExitTask`, `KillProcess`, `SetTaskPri`, `SetAffinity` | There is no fork. The ELF loader runs in the parent's `dos.library`. |
| Signals and waiting | `Wait`, `Signal`, `SetSignal`, `WaitAddr`, `WakeAddr` | Futex-style waiting is the basis of the trap-free fast path (§4). |
| Ports and connections | `CreatePort`, `PublishPort`, `OpenPort`, `ClosePort`, `PutMsg`, `DoMsg` | Rings live in user memory. The kernel creates them, tears them down and wakes waiters. |
| Time | `CreateTimer`, `ArmTimer`, `CurrentTime` | Tickless. The time counter can be read from user mode (`rdtime` / `CNTVCT_EL0`). |
| Interrupts and leases | `CreateLease`, `GrantLease`, `RevokeLease`, `MapLease`, `BindIrq`, `AckWaitIrq`, `AllocDmaArea` | A lease bundles MMIO pages, IRQs, an IOMMU context and a doorbell (§5). |
| Handles | `CloseHandle`, `DupHandle`, `SendHandle` | Handles are generation plus index, in one system-wide table (as in 3DO and Omega). |
| Diagnostics | `KLog`, `QuerySystem` | |

Not in the kernel: filesystems, ELF loading, device drivers, GPU and NPU scheduling, and power and clock policy. The kernel does keep an early console and the kernel log.

## 2.3 Process model

**Proposed. Inherited from Omega.**
- A **process** is a protection domain plus everything it owns. A **task** is the unit that gets scheduled. A process holds one or more tasks. The process, not the task, is the unit of failure ([§12.4](12-let-it-crash.md#124-the-process-is-the-unit-of-failure), proposed).
- Each task has a fixed user stack with an unmapped guard region, and its own kernel stack.
- Exit reclaims resources in this order:
  1. Stop the tasks. A task that is inside a kernel call is killed when it reaches a safe point; omega learned this when a test leaked 441 pages without it.
  2. Return unreplied messages with `ERR_PORT_DIED`.
  3. Send death notices to peers.
  4. Revoke leases. This drains or resets coprocessor queues (§5.4), and runs each device lease's stop method: bus mastering off, a reset asserted, or the power domain off (§11.4, proposed).
  5. Free Areas. Areas tied to a device lease are quarantined until the stop method has run, and are never reused if the device can't be stopped (§11.4, proposed).
  6. Wake futex waiters (§12.5, proposed).
  7. Free the domain and its ASID.

## 2.4 Servers (initial set)

| Server | Role | First board |
|---|---|---|
| `power` | Clocks, regulators, resets and power domains, using SCMI (CIX), the RK806 PMIC with the CRU (RK3588), the K1 PMU, or RPMh (Q8B). The hwdb asks `power` to turn a node on before it grants the node's MMIO lease, so a driver never touches an unpowered device (§11.5; built in V5 step 2 for the K1 and the CIX). CIX raises SError if anything reads a GPU register too early. | all |
| `dos` + handlers | Namespace and assigns. The FAT32 handler is needed for the EFI system partition. Later a modern native filesystem: journaled or CoW metadata, extents and checksums, but no snapshots or encryption (**VQ14**, **VQ14a**). | RV2 |
| `blk` drivers | SDHCI, eMMC, NVMe (PCIe), and later UFS. | RV2 / RK3588 |
| `display` | Scanout through the DPU, VOP2, Linlon or MDSS: screens and planes, flips at vblank. Built for the K1 and the CIX on the firmware's mode (V5 step 3). | RV2 (Omega A2 already has flips and a sprite plane) |
| `input` | USB HID through an xHCI/DWC3 driver. | CIX |
| `audio` | Real-time SIMD voice/stream mixer, Paula-style precedence and exclusive DMA leases over HDA (CIX ALC269), I²S (RK3588/K1) and `virtio-snd` ([§15](15-audio-dsp.md), proposed). | QEMU / CIX |
| `net` | RTL8125 (PCIe), the K1 EMAC, and a TCP/IP stack (**VQ15**). | later |
| `gpu-*` | One per GPU family, granting queue leases (§5). | CIX (Panthor, G720), then RK3588 |
| `npu-*` | Per NPU family. | CIX / RK3588 |

## 2.5 Boot

**Proposed.** All boot paths converge on one kernel entry contract.

```
RV2:      BROM → U-Boot SPL → OpenSBI (M) → U-Boot (S) → booti planar.bin      [FDT]
RK3588:   BROM → TPL/SPL → TF-A (EL3) → U-Boot → booti planar.bin              [FDT]
CIX P1:   UEFI (EL2, VHE) → planar.efi stub                                    [ACPI only]
Q8B:      Qualcomm XBL/UEFI → planar.efi stub at EL1 under hypervisor          [ACPI + DTB]
QEMU:     -kernel planar.bin (riscv64 virt / aarch64 virt)                     [FDT]
```

Kernel entry contract (`arch_entry`):
- The CPU is at S-mode, EL1 or EL2. The AArch64 entry code drops from EL2 to EL1 (**VQ11**).
- The MMU is off.
- One boot hart or CPU.
- One register holds a pointer to the boot description: an FDT, or a boot-information block (`kernel/bootinfo.h`) that holds the EFI memory map, the DTB and the RSDP if the firmware had them, the GOP framebuffer, and the command line.
- On AArch64 the image runs wherever it's loaded. It runs at the direct-map address of its physical one, and the entry code applies its relative relocations once the MMU is on.

**Built (V3 step 1).** The EFI stub (`arch/aarch64/efi.c`) does the following:
1. Copies the image to a 2 MiB boundary.
2. Collects the configuration tables, the GOP framebuffer and the load options.
3. Calls `ExitBootServices` with the final memory map.
4. Cleans the image from the caches, turns the MMU off, and enters the copy as any loader would.

**Built (V3 step 2).** The kernel reads ACPI's static tables as a second source (`kernel/acpi.c`), and `kernel/hw.c` chooses between ACPI and the device tree: the DTB, unless `hw=acpi`.

**Built (V3 step 4).** Each CPU has a core class: its core type, from MIDR (or RISC-V's `marchid`), read on the CPU itself. The PPTT's clusters are a cross-check. A class's capacity is measured at boot, by timing one fixed workload on every CPU; the fastest class gets 1024. Placement fills idle big cores first, then idle little ones, then goes where a task gets the most capacity per task. On a symmetric machine that is the old least-loaded rule.

**Built (V3 step 6).** VQ3 is decided: the identity layout (docs §3.1, [vq3-results.md](vq3-results.md)). `arch_tlb_count_start` and `arch_tlb_count_read` count user mode's TLB misses on a CPU, through the PMU on AArch64 and the SBI PMU on RISC-V. The global layout was then deleted, with `layout=` (v3-plan decision 3): an Area is one physically contiguous run, at VA = PA.

**Built (V4 step 1).** The kernel calls drivers need ([v4-plan](v4-plan.md)): processes made by processes, which never outlive their creator's handle; `CurrentTime` and `ArmTimer`; device leases (`kernel/lease.c`), created only with the device root, granted to one driver at a time, with lines delivered as signals and DMA Areas quarantined when a driver dies with them; and `MapFirmware` for the firmware's tables. `GrantLease` replaces `SendHandle` for now: hwdb keeps the leases it hands out.

**Built (V4 step 2).** hwdb's device graph (`servers/hwdb/`), run on the host for now: a device tree source and a static ACPI source, quirks as C tables, matching by rank with `make check` refusing ties, and binding in dependency order that looks again only at the consumers of a node that binds. It's tested on the QEMU trees, the RV2's U-Boot tree and the CIX's tables, each against its expected report ([v4-plan](v4-plan.md)).

**Built (V4 step 3).** hwdb runs as a server (`servers/hwdb/server.c`). It builds the graph from the firmware's description, groups devices that share pages into one lease each, and starts each lease's driver in graph order. Drivers link `drv.library` (`lib/drv`), and the first three are in `drv/`: two RTCs and a virtio-mmio bus driver. A driver that dies is checked, stopped, revoked and restarted, with backoff and a cap. The kill test runs at every QEMU boot ([v4-plan](v4-plan.md)).

**Built (V4 step 4).** acpid (`servers/acpid/`) runs ACPICA, unchanged, in its own process: hwdb starts it with the firmware's tables mapped in (`GrantFirmware`), adds the devices it describes to the graph before matching, and checks each register window its AML asks for. On QEMU the same drivers bind through ACPI as through the device tree ([v4-plan](v4-plan.md)).

**Built (V3 step 5).** On AArch64, as on RV64, a kernel call saves only what the calling convention keeps, and zeroes the scratch registers on the way out. On the CIX a null kernel call is 258 cycles on an A720 and 586 on an A520, and a `DoMsg` round trip 1,533 and 3,366 (the RV2: 469 and 3,209). `make PROFILE=1` profiles both ISAs.

**Built (V3 step 3).** One `uefi-a64` image runs on the CIX P1, with no board-specific code: the board's differences are ACPI quirk data. Leaving EL2, the entry code writes every EL2 control that can trap EL1 whole, because on a core just started by PSCI they're UNKNOWN. The direct map covers 1 TiB. The GTDT's SBSA watchdog is kept armed, so a hang resets the board.

The stub is part of the kernel image, not a separate unit. The image header is both an arm64 `Image` header and a PE/COFF header, so one file boots through `booti`, QEMU's `-kernel` and UEFI. The stub runs before relocation, so it may use only PC-relative addresses. The build checks this.

Hardware description: **VQ7. Needs a design session.** ACPI and FDT are both first-class sources:
- **ACPI** on the newer boards: Q8B, CIX P1, and the SpacemiT K3. Most SBCs from the last year ship ACPI.
- **FDT** on the older ones: RV2 and RK3588.

The working assumption is that both sources feed **one hardware model**: a device graph whose devices carry resources (MMIO, IRQ, DMA, clocks, power). Drivers bind to that graph, never to ACPI or FDT directly.

The kernel parses only what early boot needs:
- the memory map;
- the interrupt controller, from MADT or the DT;
- timers;
- the console, from SPCR or `stdout-path`;
- the CPU topology.

The session must still decide:
- where the rest of the parsing happens;
- how AML is executed, if at all;
- what the Q8B uses for power, since its ACPI power methods are Windows PEP.

## 2.6 arch and plat contract

**Decided: Omega's model.**
- Portable code lives in `kernel/`, `lib/`, `servers/`, `drivers/` and `include/planar/`, and is never compiled per platform with `#ifdef`.
- `arch/<isa>/` implements `kernel/arch.h`. `plat/<board>/` implements `kernel/plat.h`.
- Inline hot paths go in `arch/<isa>/include/planar/arch_inline.h`.
- A missing function is a link error.
- `make check` enforces the no-`#ifdef` rule.

What changes from Omega:
- `arch_domain_*` is defined in terms of a **protection back end** (§3.5) rather than assuming a page table.
- `plat_*` gains SMP bring-up (`plat_cpu_start`, using SBI HSM or PSCI), an IPI hook, and a description of the CPU topology (clusters, core classes).
- One `plat/` per SoC, not per board. Boards are data: a DTB and a small `board.mk`.

## 2.7 SMP

**Decided (VQ8, 2026-10-19): SMP from the first kernel milestone.** Getting the scheduler and messaging right on all cores early pays off later.
- Every target has 8–12 cores, and the paper's IPC story depends on crossing cores.
- Per-CPU run queues. Tasks are placed by core class, with priority bands mapped to big or little cores and the policy coming from user space.
- Fine-grained locks from day one. There is no big kernel lock to retire later.
- Omega's single-core v1 has already validated the data structures it would build on.
