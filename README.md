# Planar

Planar is an operating system for one person and one machine. It turns a modern 64-bit single-board computer into something that feels as direct as an Amiga: small, fast and fully understood, without the protection holes that came with that.

- **One address space.** Every program sees the same memory map, so a pointer means the same thing in every process and to every device. Data is shared by passing its address, not by copying or serialising it.
- **Messages without copies.** Programs talk through Exec-style ports and messages that travel by reference. When the other side is already running, a message never enters the kernel.
- **Crashes stay contained.** Each process runs in its own protection domain. A bad pointer or a dead driver takes down that process and nothing else, and the kernel reclaims everything it owned.
- **The GPU and NPU belong to the app.** Applications put work straight into the coprocessors' firmware queues and ring the doorbell. Neither the kernel nor a server sits in the data path.
- **Small kernel, everything else in user mode.** The kernel schedules, protects and routes messages. Drivers, filesystems, display and networking are ordinary programs.
- **Real hardware first.** It targets RISC-V and Arm boards with real GPUs and NPUs: the Orange Pi RV2, CIX P1, RK3588 and Qualcomm SC8280XP. Every cost claim is measured on the board.

Today Planar boots on QEMU (riscv64 and aarch64), on the Orange Pi RV2, and on the CIX P1 (Orange Pi 6 Plus) through UEFI and ACPI. It has SMP scheduling across core classes (all 12 of the CIX's big and little cores), user processes in their own protection domains, and zero-copy IPC with connections, rings and `DoMsg` ([V1](docs/v1-plan.md), [V2](docs/v2-plan.md), [V3](docs/v3-plan.md)). Its address space is identity-mapped: an address is the physical address ([VQ3](docs/vq3-results.md)). Its drivers run in user mode: hwdb starts them in its device graph's order and restarts them when they're killed, and under ACPI, acpid runs ACPICA in user mode to describe the namespace, so the same drivers bind as through the device tree. Each board boots from its own storage (NVMe on the CIX, the SD card on the RV2) through FAT32 to a Shell on its screen ([V4](docs/v4-plan.md)).

The design builds on [Omega](../omega/) and [PlanOmega](../PlanOmega/), which proved the single address space and Exec-style IPC on the RV2. It started from the *Design Specification for a Lightweight, Flat-Memory Operating System* ([vision](docs/00-vision.md)).

## One tree, every platform

The structure follows omega's:
- **Portable code** lives in `kernel/`, `lib/`, `servers/`, `drv/` (user-mode drivers), `drivers/` (the kernel's own) and `include/planar/`, and exists once.
- **Code from elsewhere**, kept unchanged, lives in `third_party/`: ACPICA, which acpid runs.
- **Architecture code** lives in `arch/<isa>/` and implements `kernel/arch.h`.
- **SoC code** lives in `plat/<soc>/` and implements `kernel/plat.h`.
- **No `#ifdef`s on architecture or platform in portable code.** `make check` enforces this. A port that's missing an `arch_*` or `plat_*` function fails to link.

| Platform | Arch | What it is |
|---|---|---|
| `qemu-virt-rv64` (default) | `rv64` | QEMU's riscv64 `virt` machine under OpenSBI, S-mode |
| `qemu-virt-a64` | `aarch64` | QEMU's aarch64 `virt` machine, EL1. `QEMU_EL2=1` enters at EL2 to test the drop to EL1. |
| `ky-x1` | `rv64` | Orange Pi RV2 (SpacemiT K1), booted over its serial console through U-Boot (`tools/serial-boot.py`) |
| `uefi-a64` | `aarch64` | One UEFI image for Arm boards, with ACPI or a device tree. Today the CIX P1, installed and booted from the board's FreeBSD over ssh (`tools/efi-boot.py`). `EFI=1` on `qemu-virt-a64` boots the same image through QEMU's edk2. |
| `hosted` | `host` | A host executable. It can be given a `.dtb` to parse, or a board's ACPI tables (`ACPI_BOARD=cix-p1`). |

Both QEMU platforms share `plat/qemu-virt/`. The hardware comes from the device tree or ACPI, whichever the firmware gives (`kernel/hw.c`).

## Building

Needs `make`, Python 3, QEMU, and Homebrew's `llvm@22` and `lld@22`. The toolchain is pinned by absolute path in `mk/toolchain.mk`, so a Swiftly or Xcode clang earlier on `PATH` doesn't matter.

```sh
make                          # PLATFORM=qemu-virt-rv64: build/qemu-virt-rv64/planar.bin
make run                      # boot it in QEMU (Ctrl-A X quits)
make PLATFORM=qemu-virt-a64 run
make PLATFORM=hosted run      # or: build/hosted/planar some.dtb
make PLATFORM=... test        # boot and check the transcript (tools/boot-check.py)
make test-all                 # every platform that runs here: 10 boots, compare-hw, hwdb-test
make check                    # portable code must not test arch or platform; no match-table ties
make hwdb-test                # hwdb's device graph on the captured trees and tables
make PLATFORM=ky-x1 test      # the RV2, over the serial console
make PLATFORM=uefi-a64 test   # the CIX P1, through its FreeBSD (BOARD=root@…)
make vq3                      # the VQ3 soak of Areas under memory pressure
```

| Board | SoC | ISA |
|---|---|---|
| Orange Pi RV2 | SpacemiT K1 (8× X60) | RV64GCV, Sv39 |
| Orange Pi 5 Plus | Rockchip RK3588 (4× A76 + 4× A55) | Armv8.2-A |
| Orange Pi 6 Plus | CIX P1 / CD8180 (8× A720 + 4× A520) | Armv9.2-A |
| Radxa Dragon Q8B | Qualcomm SC8280XP (4× X1C + 4× A78C) | Armv8.2-A |

## Reference manuals

[manuals/](manuals/README.md) is Planar's version of the Amiga ROM Kernel Reference Manuals. It covers what's built and how to program against it: Libraries, Devices, Autodocs, Includes, Hardware. The design documents below cover what Planar should be.

## Planar and the web

Don't expect a modern browser. Networking isn't in any roadmap phase yet. The `net` server (wired Ethernet only: RTL8125 and the K1 EMAC, no Wi-Fi) is marked "later", and its TCP/IP stack is still open (VQ15). Nothing yet covers TLS, DNS, an entropy source, or reading the real-time clock, and certificates can't be checked without the time. When those arrive, the realistic target is a curl-class client: an app fetching HTTPS from endpoints you chose, built on lwIP, a small TLS library and a thin POSIX layer. Chromium, Firefox and WebKit are out of reach. They need POSIX, Mesa-class GPU userspace and a sandbox, and the vision rules out all three as design drivers (§0.3). Treat anything that serves the internet with care too: the design has no adversary model (P1), so there's no ASLR, no stack canaries and no sandboxing, and some drivers get unconfined DMA. Protection domains contain bugs, not attackers. Put Planar behind a trusted LAN or a proxy.

## Design documents

Each design decision is marked **decided**, **proposed** or **open**, as in PlanOmega. Open questions are numbered **VQ*n*** and collected in [questions.md](docs/questions.md).

1. [Vision and principles](docs/00-vision.md): what Planar is and isn't, and how it relates to Omega, HellFire and the earlier VectraOS corpus.
2. [Review of the vision paper](docs/01-spec-review.md): each claim checked against the hardware. Says what survives, what has to change, and what the paper leaves out.
3. [Architecture overview](docs/02-architecture.md): layers, kernel and server split, boot, the arch/plat contract.
4. [Memory and protection](docs/03-memory.md): the identity single address space (VQ3, decided by measurement), protection domains, and the PMP and MPU back ends.
5. [IPC](docs/04-ipc.md): ports, messages, rings, signals and doorbells.
6. [Coprocessors](docs/05-coprocessors.md): queue leases for GPU and NPU, board by board.
7. [Platforms](docs/06-platforms.md): board profiles, boot chains, hazards.
8. [Language, toolchain and tree](docs/07-language-toolchain.md): C23 (decided), SIMD policy, build, naming.
9. [Roadmap](docs/08-roadmap.md): phased plan and board order.
10. [Hardware description](docs/09-hardware-description.md): ACPI and FDT, the hwdb device graph, AML isolation (VQ7, decided).
11. [Driver model](docs/11-driver-model.md): lessons from other systems' driver models, applied to user-mode drivers (proposed).
12. [Fault tolerance: let it crash](docs/12-let-it-crash.md): Erlang-style crash recovery, supervision and its hardware limits (proposed).
13. [Workbench and desktop environment](docs/13-workbench.md): Intuition, zero-copy window surfaces, visual design system, widget toolkit and native iconography (proposed).
14. [OpenType and TrueType font engine](docs/14-font-engine.md): clean-room `font.library` from the spec for English layouts, stubbed non-English/colour features, and host-side FreeType/HarfBuzz test oracle (proposed).
15. [Audio and DSP subsystem](docs/15-audio-dsp.md): scaling up Amiga `audio.device`, AHI and `realtime.library` with zero-copy voice/stream rings, SIMD `dsp.library`, hardware DSP queue leases and click-free crash recovery (proposed).
16. [RTG for modern DPUs and GPUs](docs/16-rtg-display-gpu.md): updating Commodore's original V36–V40 RTG and AAA/Hombre display architecture (`AllocBitMap`, `MonitorSpec`, `View`/`ViewPort`/`MrgCop`/`LoadView`, `ChangeScreenBuffer`) for multi-plane DPUs and user-mode GPU queue leases (proposed).
17. Phase plans, each with what was built and measured: [V1](docs/v1-plan.md) (the SAS core, and the kernel's upper-half decision), [V2](docs/v2-plan.md) (IPC), [V3](docs/v3-plan.md) (the CIX P1, ACPI, core classes), [V4](docs/v4-plan.md) (drivers and servers, storage, the Shell).
18. [VQ3 results](docs/vq3-results.md): identity vs. global-VA, measured on QEMU, the RV2 and the CIX.
19. [Code review](docs/10-code-review.md): the adversarial review after V2, and its fixes (V2.5).
20. [Open questions](docs/questions.md): the question register.

## Status

| Phase | Done |
|---|---|
| V0. Tree, toolchain, device-tree console | 2026-10-19 |
| V1. Single address space, SMP, tasks, signals, Areas (QEMU and the RV2) | 2026-10-19 |
| V2. IPC: connections, rings, `DoMsg`, death notices; then hardening (V2.5) | 2026-10-20 |
| V3. The CIX P1 through UEFI and ACPI, 12 cores in two classes, Arm costs, VQ3 | 2026-10-20 |
| V4. User-mode drivers under hwdb and acpid, the kill test, NVMe and SD, FAT32, a Shell on the framebuffer | 2026-10-20 |

Decided so far (the full register is [questions.md](docs/questions.md)):
- Copy what we can from omega (VQ1).
- The project is named Planar (VQ2).
- The identity single address space (VQ3, by measurement). The global layout was deleted after V3.
- The kernel in the upper half, with a direct map (VQ4).
- ACPI and FDT through the `hwdb` device graph, with ACPICA in user space (VQ7).
- SMP from V1 (VQ8).
- C23 plus assembly, with no SIMD in the kernel (VQ12).
- FAT32 plus a modern native filesystem (VQ14).
- Board order RV2 → CIX → RK3588 → Q8B (VQ19).

Next: V5 ([plan](docs/v5-plan.md), proposed): GPU queue leases (a CSF job on the CIX's G720 with no kernel or server in the data path, and the RV2's BXE-2-32 through a server queue), native scanout, `power` and USB input. Then V6, the Workbench ([roadmap](docs/08-roadmap.md)).
