# Hardware

*Planar Reference Manuals, volume 5.* Where the RKM's *Hardware Reference Manual* described one machine's chips, this volume describes the machines Planar runs on, and the contract a new port implements.

**Outline.** The facts exist in the design set ([docs §6](../../docs/06-platforms.md), [§9](../../docs/09-hardware-description.md)) and in `arch/` and `plat/`. This volume will gather them for someone porting Planar or writing a driver.

## Expected chapters

| # | Chapter | Source |
|---|---|---|
| 1 | The port contract: what `kernel/arch.h` and `kernel/plat.h` require, and `make check` | `kernel/arch.h`, `kernel/plat.h`, `tools/check-portable.sh` |
| 2 | The trap ABI and user-mode entry, per ISA: a kernel call keeps what the calling convention keeps and returns the scratch registers zeroed (`callregs` checks both) | `planar/syscalls.h`, `arch/<isa>/user/`, `conformance/callregs.c` |
| 3 | RISC-V (`rv64`): Sv39, SBI, PLIC, lazy FP and vector state | `arch/rv64/` |
| 4 | AArch64: EL2 to EL1, GICv3, TTBR0/TTBR1, PAN, the kernel-call fast path (`svc_entry`), the cycle counter | `arch/aarch64/` |
| 5 | QEMU `virt`, rv64 and aarch64 | `plat/qemu-virt/` |
| 6 | Orange Pi RV2 (SpacemiT K1): serial boot, watchdog, the board's quirks | `plat/ky-x1/`, `tools/serial-boot.py` |
| 7 | CIX P1 (UEFI, ACPI, `BootNext` runs, the EL2 controls, the SBSA watchdog, GIC-700 erratum 2941627); RK3588, SC8280XP | `plat/uefi/`, `tools/efi-boot.py`, `docs/boards/cix-p1/`; **planned**: RK3588 V6, SC8280XP V7 |
| 7a | Firmware descriptions: the device tree and ACPI, `hw=`, the `hw:` facts, board quirks | `kernel/hw.h`, `kernel/acpi.h`, `kernel/fdt.h`, `docs/boards/` |
| 7b | Core classes: core type from MIDR or `marchid`, clusters from the PPTT, capacity measured at boot, and placement by capacity | `kernel/cpuclass.h`, `kernel/sched.c` |
| 7c | What the kernel's paths cost on each core class (`V3.5 costs`), TLB misses per layout (`V3.6 tlb`, `arch_tlb_count_*`: PMUv3 events on AArch64, the SBI PMU on RISC-V), and the profiler (`make PROFILE=1`) | `kernel/selftest.c`, `kernel/arch.h`, `kernel/prof.h`, docs/v3-plan.md steps 5 and 6 |
| 8 | The program image format (`PLNR` header) | `arch/<isa>/user/crt0.S`, `kernel/process.c` |
