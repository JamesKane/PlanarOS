# VectraOS (new line)

A lightweight single-user OS for the 64-bit SBCs in the office. All processes share one flat address space. IPC is passing pointers in Amiga Exec style. Coprocessors are driven as "firmware as hardware". The starting point is *Design Specification for a Lightweight, Flat-Memory Operating System* (the "vision paper"). The design leans on [Omega](../omega/) and [PlanOmega](../PlanOmega/), which have already proved a single address space and Exec-style IPC on the Orange Pi RV2.

V0 is complete: one tree boots to a serial console on QEMU for both ISAs, and runs as a host process.

## One tree, every platform

The structure follows omega's:
- **Portable code** lives in `kernel/`, `lib/`, `servers/`, `drivers/` and `include/vectra/`, and exists once.
- **Architecture code** lives in `arch/<isa>/` and implements `kernel/arch.h`.
- **SoC code** lives in `plat/<soc>/` and implements `kernel/plat.h`.
- **No `#ifdef`s on architecture or platform in portable code.** `make check` enforces this. A port that's missing an `arch_*` or `plat_*` function fails to link.

| Platform | Arch | What it is |
|---|---|---|
| `qemu-virt-rv64` (default) | `rv64` | QEMU's riscv64 `virt` machine under OpenSBI, S-mode |
| `qemu-virt-a64` | `aarch64` | QEMU's aarch64 `virt` machine, EL1. `QEMU_EL2=1` enters at EL2 to test the drop to EL1. |
| `hosted` | `host` | A host executable. It can be given a `.dtb` to parse. |

Both QEMU platforms share `plat/qemu-virt/`. Everything V0 needs, including the console, comes from the device tree.

## Building

Needs `make`, Python 3, QEMU, and Homebrew's `llvm@22` and `lld@22`. The toolchain is pinned by absolute path in `mk/toolchain.mk`, so a Swiftly or Xcode clang earlier on `PATH` doesn't matter.

```sh
make                          # PLATFORM=qemu-virt-rv64: build/qemu-virt-rv64/vectra.bin
make run                      # boot it in QEMU (Ctrl-A X quits)
make PLATFORM=qemu-virt-a64 run
make PLATFORM=hosted run      # or: build/hosted/vectra some.dtb
make PLATFORM=... test        # boot and check the transcript (tools/boot-check.py)
make test-all                 # all three platforms
make check                    # portable code must not test arch or platform
```

| Board | SoC | ISA |
|---|---|---|
| Orange Pi RV2 | SpacemiT K1 (8× X60) | RV64GCV, Sv39 |
| Orange Pi 5 Plus | Rockchip RK3588 (4× A76 + 4× A55) | Armv8.2-A |
| Orange Pi 6 Plus | CIX P1 / CD8180 (8× A720 + 4× A520) | Armv9.2-A |
| Radxa Dragon Q8B | Qualcomm SC8280XP (4× X1C + 4× A78C) | Armv8.2-A |

## Design documents

Each design decision is marked **decided**, **proposed** or **open**, as in PlanOmega. Open questions are numbered **VQ*n*** and collected in [questions.md](docs/questions.md).

1. [Vision and principles](docs/00-vision.md): what Vectra is and isn't, and how it relates to Omega, HellFire and the earlier VectraOS corpus.
2. [Review of the vision paper](docs/01-spec-review.md): each claim checked against the hardware. Says what survives, what has to change, and what the paper leaves out.
3. [Architecture overview](docs/02-architecture.md): layers, kernel and server split, boot, the arch/plat contract.
4. [Memory and protection](docs/03-memory.md): single address space (identity vs. global-VA pending measurement), protection domains, and the PMP and MPU back ends.
5. [IPC](docs/04-ipc.md): ports, messages, rings, signals and doorbells.
6. [Coprocessors](docs/05-coprocessors.md): queue leases for GPU and NPU, board by board.
7. [Platforms](docs/06-platforms.md): board profiles, boot chains, hazards.
8. [Language, toolchain and tree](docs/07-language-toolchain.md): C23 (decided), SIMD policy, build, naming.
9. [Roadmap](docs/08-roadmap.md): phased plan and board order.
10. [Hardware description](docs/09-hardware-description.md): ACPI and FDT, the hwdb device graph, AML isolation (VQ7, decided).
11. [Open questions](docs/questions.md): the question register.

## Status

Decided on 2026-10-19:
- Copy what we can from omega (VQ1).
- "vectra" is the working name (VQ2).
- SMP from V1 (VQ8).
- C23 plus assembly, with no SIMD in the kernel (VQ12).
- FAT32 plus a modern native filesystem (VQ14).
- ACPI and FDT through the `hwdb` device graph, with ACPICA in user space (VQ7).
- Board order RV2 → CIX → RK3588 → Q8B (VQ19).

Next:
- The VQ3 measurements in V1.
- Dump the CIX ACPI tables (§9.6).
- Start V0.
