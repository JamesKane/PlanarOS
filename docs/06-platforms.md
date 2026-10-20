# 6. Platforms

Facts are cited to where they were measured. *(verify)* marks anything that hasn't been confirmed on the board yet.

## 6.1 Summary

| | Orange Pi RV2 | Orange Pi 5 Plus | Orange Pi 6 Plus | Radxa Dragon Q8B |
|---|---|---|---|---|
| SoC | SpacemiT K1 | RK3588 | CIX P1 (CD8180) | SC8280XP |
| CPU | 8× X60 @1.6 GHz, 2 clusters | 4× A76 + 4× A55 | 8× A720 + 4× A520 | 4× X1C + 4× A78C |
| ISA | RV64GCV, RVA22, VLEN 256 | Armv8.2-A | Armv9.2-A, SVE2 | Armv8.2-A (no SVE) |
| Entry level | S-mode (OpenSBI 1.3, SBI 1.0) | EL2 likely *(verify)* | EL2, VHE | **EL1 under Qualcomm hyp** |
| Firmware → OS | U-Boot `booti` + FDT | TF-A + U-Boot `booti` + FDT | UEFI, **ACPI only** | UEFI, ACPI + DTB |
| IRQ | PLIC (159 sources, 7 prio) + CLINT | GICv3 (GIC-600) + 2 ITS | GICv3/4.1 + ITS, 512 SPI | GICv3 |
| Timer | `rdtime` 24 MHz, Sstc | Generic timer | Generic timer; per-core stops in deep idle, use CIX GPT | Generic timer; per-core not always-on, use MMIO timer |
| GPU | IMG BXE-2-32 | Mali-G610 (CSF) | Immortalis-G720 MC10 (CSF) | Adreno 690 |
| NPU | In-core AI ISA *(verify)* | RKNN ×3 | Zhouyi X2, 30 TOPS | Hexagon v68 HTP |
| Display | K1 DPU (Omega A2: flips, sprite) | VOP2 + HDMI-QP | Linlon DP ×5 (komeda) | MDSS DPU |
| IOMMU | None known | Per-block Rockchip IOMMUs | SMMUv3 ×2 | MMU-500 ×2 + SMMUv3, hyp-owned |
| Serial | UART0 `0xd401_7000`, 16550 32-bit | UART2 (debug) | 10-pin, 3.3 V, no SPCR | 1.8 V header |
| Prior work | Omega A0–A2, A1 | VectraOS-rust plan (no metal) | AbyssBSD: GPU, NPU, VPU, display working | AbyssBSD: boots, GPU, display, NPU |
| Main sources | `PlanOmega/docs/alpha/target.md` | `OS/VectraOS-rust/docs/spec/board-orangepi5plus.md` | `OS/AbyssBSD/docs/boards/orangepi-6-plus/` | `OS/AbyssBSD/docs/boards/radxa-dragon-q8b/` |

**Candidate: SpacemiT K3 board (VQ21).** RISC-V with ACPI firmware. It's currently not booting and would need to be revived. It would be the RISC-V test case for the ACPI path. Its core type, ISA profile and interrupt controller (probably AIA: APLIC and IMSIC) are all *(verify)*.

## 6.2 Hazards per board

**RV2**
- U-Boot starts the SoC watchdog and the PMIC watchdog, both at 60 s. Kick or disable both early.
- CLINT `mtime` reads all ones from S-mode. Use `rdtime`/Sstc.
- The vendor DT's 10 MHz timebase is wrong; the real value is 24 MHz.
- PMP count and SSWI have not been measured. That needs an M-mode probe, which means building our own OpenSBI (**VQ5**).

**RK3588**
- The CPU index is in MPIDR Aff1.
- ITS quirks: ForceNoShareable and erratum RK3588001.
- No FreeBSD driver exists for the CRU, RK806, VOP2 or HDMI-QP, so Linux is the only reference.
- The 2.5 GbE ports are RTL8125 on PCIe, so networking needs PCIe first.

**CIX P1**
- ACPI only, and the firmware ships no DTB.
- The DSDT's RSVL carve-outs are reported as free memory; treat them as reserved.
- The SBSA watchdog refresh frame is broken; refresh through `WOR`.
- GPU power-on order (§5.3).
- Reported SMMUv3 event-queue IRQ storm.
- Arm erratum 2941627.

**Q8B**
- The hypervisor resets the SoC on bad CBAR or SMR writes, or a bad GMU mask.
- Never PAS-shutdown a running DSP.
- The ACPI clocks are Windows PEP (`QCOM0617`), which we can't use. Power needs a DT-based RPMh path, or leaving what UEFI set up.
- Scanout must be contiguous and below 4 GB.
- UEFI variables live in TrustZone, and there is no RTC.

## 6.3 QEMU

`qemu-virt` for riscv64 and aarch64 is the first platform for both ISAs, and the target CI runs on.

QEMU flushes the TLB on every `satp` write and has none of the vendor IP. It checks correctness only. Costs come from the boards (P8).

## 6.4 Hosted

Omega's `hosted` platform (ucontext) carries over: kernel logic, IPC and servers run as a macOS/Linux process for fast iteration and sanitizers.
