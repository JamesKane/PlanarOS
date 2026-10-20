# 8. Roadmap

**Proposed.** Each phase ends with a conformance transcript on QEMU *and* on a board, as in Omega's A0/A1/A2 method. Every check is mutation-tested.

**Every phase's exit also includes the [manuals](../manuals/README.md).** Before a phase is called done:
- each call it added or changed has an autodoc;
- each chapter it affected is rechecked and its *Checked against* line moved to the phase;
- anything it built that a manual marked **Planned** loses the marker.

The manuals may lag within a phase, but not across one.

| Phase | Goal | Platforms | Exit criteria |
|---|---|---|---|
| **V0. Tree** ✅ 2026-10-19 | Tree, Make, toolchain pin (LLVM 22), `check-portable`; FDT parser and FDT-driven early console (8250, PL011); EL2→EL1 drop | qemu-virt rv64 + aarch64, hosted | Both ISAs boot to the serial console from one tree; `make test-all` passes |
| **V1. SAS core + SMP** ✅ 2026-10-19 | PMM zones, **both** domain layouts (identity and global-VA) behind `arch_domain_*`, tasks, SMP scheduler with per-CPU run queues, signals, handles | qemu, **RV2** | Omega A1 acceptance tests pass on all 8 harts in both layouts. Context switch and `Signal` costs measured. VQ3 soak and cost data collected |
| **V2. IPC** ✅ 2026-10-20 | Connections, arenas, SPSC rings with the `waiting` protocol, `DoMsg` handoff, death notices. Then hardening after an adversarial review (V2.5) | qemu, RV2 | Zero-trap streaming measured; `DoMsg` ≤ Omega's 3.4k cycles. **Met:** 24–32 kernel calls per 1,000 streamed messages; `DoMsg` 3,209 cycles (identity), 3,283 (global) on the RV2 ([v2-plan](v2-plan.md)) |
| **V3. First Arm board, ACPI** ✅ 2026-10-20 | EFI stub, static ACPI parser (one UEFI image, quirks as data), GIC-700 erratum, SBSA watchdog, 12-core PSCI SMP, core classes, AArch64 kernel-call fast path ([v3-plan](v3-plan.md)). The CIX GPT is deferred: idle is WFI only | qemu (edk2), **CIX P1** | V1 and V2 suites pass on all 12 cores in both layouts, and on QEMU through ACPI and FDT alike; per-core costs (A720, A520) compared with the RV2. **VQ3 decided** using RV2 and Arm data |
| **V4. Hardware model, drivers, servers** | `hwdb` (FDT and ACPI sources, quirks), `acpid` (ACPICA), `power` (native SCMI, `acpi-pr`), leases and IRQ binding; SD/eMMC/NVMe, FAT32, display scanout (K1 DPU, Linlon) ([v4-plan](v4-plan.md), proposed) | RV2, CIX | Boot from storage to a shell on the framebuffer on both boards. The same drivers bind through FDT and through ACPI. Drivers bind in dependency-graph order and restart after a kill; the kill test passes (§11.10). 4 KiB read IOPS and latency measured at queue depths 1–32 (§11.8) |
| **V5. Queue leases** | `gpu-panthor`: CSF firmware boot, CSG slots, user doorbells. Then the Zhouyi NPU server | CIX (G720) | An app submits a CSF job and waits on a sync object with no kernel or server in the data path |
| **V6. RK3588** | `booti` path, RK806/CRU power providers, VOP2, RKNN | **RK3588** | V5 demo running unchanged on the G610 through the FDT path |
| **V7. Q8B** | EFI stub at EL1 under the hypervisor; ACPI plus DTB merge; RPMh power; Adreno server; FastRPC (VQ16) | **Q8B** | Boots to a shell; GPU queue lease through the server provider |
| **V8. PMP profile** | Planar M-mode monitor or an OpenSBI fork; `pmp` back end | RV2 | Paper's PMP model demonstrated with its region budget measured |

Board order (**VQ19**, decided): RV2 → CIX → RK3588 → Q8B.
- **RV2** reuses Omega's facts.
- **CIX** proves the ACPI path, UEFI and Panthor early. It has no hypervisor and the cleanest ACPI.
- **RK3588** proves the FDT path on Arm and reuses the Panthor server.
- **Q8B** comes last, with the hypervisor and Qualcomm's stack.
- **K3** (VQ21) joins as soon as it's revived, as the test case for ACPI on RISC-V.
