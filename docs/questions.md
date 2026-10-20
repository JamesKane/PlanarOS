# Open questions

Each entry gives the question, where it came up, and the current leaning or decision. A **★** marks a question that blocks V0 or V1.

## Decided

| # | Decision | Date |
|---|---|---|
| VQ1 | **Copy what we can from omega**: the build, the arch/plat contract, handles, signals, ports, lazy FP/vector state and the reclamation order. Staying compatible with omega afterwards isn't a goal. | 2026-10-19 |
| VQ2 | **The project is named Planar.** It started under the working name "vectra"; the earlier VectraOS corpus keeps that name. The VQ numbers stay as they are. | 2026-10-19 |
| VQ8 | **SMP from V1.** The scheduler and messaging run on all harts or cores from the first kernel milestone, with per-CPU run queues and placement by core class. | 2026-10-19 |
| VQ12 | **C23 plus assembly.** SIMD is expected in user code (apps, servers and libraries), and the kernel is built without it. The kernel stays small. Odin isn't part of the plan. | 2026-10-19 |
| VQ4 | **The kernel stays in the upper half in both layouts**, with its direct map mapped global. Only user Areas move between VA = PA and the global VA window. See [v1-plan.md](v1-plan.md). | 2026-10-19 |
| VQ7 | **ACPI and FDT are both native sources.** The kernel parses static tables only. A user-mode `hwdb` builds one device graph. ACPICA runs unmodified in an isolated user-mode `acpid`. The Q8B merges ACPI with its firmware DTB. Board quirks are kept as data. Details in [09-hardware-description.md](09-hardware-description.md). | 2026-10-19 |
| VQ19 | **Board order: RV2, then CIX (Orange Pi 6 Plus), then RK3588, then Q8B.** The ACPI path gets proven on the second board. | 2026-10-19 |
| VQ14 | **FAT32 is required** for the EFI system partition and firmware interchange. The **native filesystem is a modern design**: journaling or copy-on-write metadata, extents and checksums are candidates. Snapshots and encryption are out of scope. The specific design is still open (VQ14a). | 2026-10-19 |

## Memory

**VQ3 ★ Identity SAS (VA = PA) or global-VA SAS (Omega). Decide from measurements.**

No commitment until V1 has numbers from the RV2 and at least one Arm board. Both layouts are built (V1 step 5) and chosen at boot with `layout=`. The **first QEMU results are in [vq3-results.md](vq3-results.md)**:
- Under memory pressure, identity fails about 0.2% of allocations even though enough memory is free.
- Global leaves physical memory about 2.8× more fragmented.
- Page tables and allocation time are about the same.

Once it's decided (V3 step 6), the losing layout is deleted after V3, in a commit of its own (v3-plan decision 3, 2026-10-20).

V1 measures:
- Largest free run per zone after a soak test that creates and destroys processes and grows and shrinks heaps.
- How often allocations fail even though enough total memory is free.
- Context switch and TLB miss cost in each layout.
- How much `IOF_LEND` and DMA setup the identity layout saves.
- How much code each layout needs in the loader and the allocator.

Raised in §3.1 and §8.

**VQ5 PMP and M-mode on RISC-V.**

Should we ever replace OpenSBI with our own M-mode monitor? *Leaning:* not before V8. The `pmp` back end stays in the contract.

Raised in §1.2 and §3.5.

**VQ6 How processes find ExecBase.**

*Leaning:* pass its address in the first argument register at process start. Under the global-VA layout a fixed address becomes possible again.

Raised in §1.6.

## Platform

**VQ9 DMA confinement.**

On boards with no usable IOMMU, can drivers be trusted with unconfined DMA? *Leaning:* yes, as long as the lease records that its DMA is unconfined.

Raised in §1.5 and §3.7.

**VQ11 Entry exception level.**

Drop from EL2 to EL1, or run at EL2 with VHE? *Leaning:* EL1 everywhere.

Raised in §2.5.

**VQ21 SpacemiT K3 board.**

Should it become a target if it can be revived? It is RISC-V with ACPI and probably AIA rather than a PLIC *(verify)*. That makes it the RISC-V counterpart to CIX for the ACPI path, and possibly a better long-term RISC-V target than the RV2.

Raised in §6.

## IPC

**VQ10 What happens when a ring is full.**

*Leaning:* the ring size is chosen per connection at `OpenPort`. `PutMsg` returns `ERR_FULL`; `DoMsg` blocks.

Raised in §4.3.

**VQ20 Message ownership.**

After `PutMsg`, is ownership a convention or enforced by the kernel? *Leaning:* convention.

Raised in §4.3.

## Language and build

**VQ18 Public type names.**

Use Omega's Amiga typedefs, or `<stdint.h>` names with CamelCase API functions? *Leaning:* `<stdint.h>`.

Raised in §7.3.

**VQ13 Hardening features.**

Should BTI, PAC and MTE be used where they exist (A720)? *Leaning:* BTI and PAC on, MTE as a debug option.

Raised in §3.6.

**VQ22 SIMD in the shared library region.**

User code is expected to use SIMD, so the shared `mem*`/`str*` and codec helpers should use it too. Should one version be chosen per CPU at boot (RVV 1.0, NEON, SVE2), or should each library ship versions selected through its interface table?

Raised in §7.4.

## Services

**VQ14a Native filesystem design.**

FAT32 is decided for the EFI system partition. For the native filesystem: write our own, or adapt an existing design? Its properties are set in VQ14 above.

Raised in §2.4.

**VQ15 Network stack.**

Write our own or port lwIP?

Raised in §2.4.

## Drivers

**VQ23 Driver restart.**

Should the hwdb restart a driver that dies, MINIX-style, from V4? *Leaning:* yes, with backoff and a cap. The node keeps its unit, and class libraries reconnect and retry what is safe to retry.

Raised in §11.10.

**VQ24 Devices with no stop method.**

When a driver dies, something must stop its device before its DMA Areas are reused. What happens for a device with no reset line, no power domain of its own, no bus-master bit and no IOMMU? *Leaning:* its Areas stay quarantined for the rest of the boot, and the hwdb logs it. Each board's device list should be checked for such devices before V4.

Raised in §11.4.

**VQ25 Where match tables live.**

An ELF note section in each driver binary, read by the hwdb from the boot image, or one central table built into the hwdb? *Leaning:* ELF notes, with `make check` failing on ambiguous matches.

Raised in §11.6.

**VQ26 Lent buffers and the IOMMU.**

On boards with an IOMMU, a client buffer lent to a driver (`IOF_LEND`) must be mapped into the device's context. Map each lent Area per request, keep mappings cached while the Area lives, or bounce through the driver's pool? *Leaning:* cache the mapping for the Area's life under the identity layout, and measure.

Raised in §11.8.

**VQ27 Storage queue leases.**

Should the filesystem hold an NVMe I/O queue pair as a lease, taking the driver out of the data path? NVMe doorbells usually share one page, so the filesystem could ring other queues' doorbells. *Leaning:* not in V4. Decide after the queue-depth-1 measurement in §11.8.

Raised in §11.8.

**VQ28 Ports across a server restart.**

When a server or driver crashes and its supervisor restarts it, `drop_port` currently deletes the port name, leaving a window where `OpenPort` returns `ERR_NOTFOUND`. Should the supervisor hold the port handle across restarts (socket-activation style), or should `OpenPort` gain a wait-for-port option? *Leaning:* supervisor-held port handles for supervised services, plus a bounded wait option on `OpenPort` for boot ordering.

Raised in §12.6.

**VQ29 Supervision tree for core servers.**

`hwdb` supervises drivers (§11.10). Who supervises `hwdb`, `power`, `dos` and `display`, and what happens if `hwdb` itself crashes? *Leaning:* a tiny `init` starts and watches the root servers; a crash in `hwdb` logs a meditation and reboots via the watchdog in V4, while filesystem handlers and leaf drivers restart in place.

Raised in §12.7.

**VQ30 Recovering `power` state after a crash.**

If the `power` server crashes and restarts while drivers are running, how does it know which power domains and clocks must stay on? *Leaning:* `hwdb` already tracks which nodes were powered before granting leases (D3, §11.5), so `hwdb` replays the active node set to `power` on reconnect.

Raised in §12.7.

## Workbench and UI

**VQ31 Direct DPU plane promotion vs. full-screen composition.**

Should `intuition` assign visible top-level windows directly to hardware DPU overlay planes when within the plane budget, or compose into a single primary scanout buffer? *Leaning:* always use the hardware cursor plane for the pointer; compose windows into one primary scanout buffer first, then promote full-screen or GPU queue-lease windows to dedicated overlay planes.

Raised in §13.1 and §13.7.

**VQ32 Icon asset representation.**

Store icons in `.info` sidecar files (works on FAT32 and the native filesystem alike) with a compact vector command stream, or as pre-rasterised `ARGB8888` bitmaps? *Leaning:* `.info` sidecar files containing a tiny binary vector stream plus a cached `64×64` bitmap.

Raised in §13.4 and §13.7.

**VQ33 Window translucency (`Shell` backdrop blur).**

How should `intuition` implement the translucent `Shell` background before GPU composition is up on every board? *Leaning:* sample from a pre-blurred copy of the static procedural backdrop on CPU composition; optional GPU blur when `intuition` holds a queue lease.

Raised in §13.5 and §13.7.

**VQ34 Base system font files (`System Sans` and `System Mono`).**

Which permissive open-source `.ttf`/`.otf` fonts should ship in the image via `#embed` for V4 (`Shell`) and V5.5 (`Workbench`)? *Leaning:* subsetted OFL builds of a geometric/humanist sans (Inter or IBM Plex Sans) and programmer monospace (JetBrains Mono or IBM Plex Mono) in the boot image, with unmodified full fonts in `tools/font-oracle/`.

Raised in §14.7.

**VQ35 Horizontal subpixel positioning (`1/4 px` bins) vs. whole-pixel glyph placement.**

Should `font.library` cache `4×` horizontal subpixel phases (`0.0`, `0.25`, `0.5`, `0.75 px`) at small UI sizes (`12–16 px`), or snap glyph origins to integer pixels? *Leaning:* `4×` horizontal subpixel bins for `System Sans` at `12–16 px`; integer origins for `System Mono` and sizes $\ge 18\text{ px}$.

Raised in §14.7.

**VQ38 Compressed / tiled framebuffer modifiers (`MOD_AFBC_16x16`, `MOD_UBWC`) vs. linear-only surfaces in V5.**

Should `AllocBitMap` default to `MOD_LINEAR` and reserve `MOD_AFBC_16x16` for explicit GPU-rendered scanout surfaces on CIX P1 (G720 + Linlon) and RK3588 (G610 + VOP2)? *Leaning:* yes; `MOD_LINEAR` (`ARGB8888` / `XRGB8888`) is the universal baseline for CPU SIMD and mixed surfaces, while `MOD_AFBC_16x16` is selected only when `BMF_GPU_ONLY | BMF_DISPLAYABLE` is requested on an AFBC-capable `friend_bitmap`.

Raised in §16.8.

**VQ39 Hardware screen dragging and multi-screen DPU plane budgeting.**

When multiple Intuition `Screen`s are visible at once (e.g. classic vertical screen dragging revealing a background screen at a different resolution or pixel format), how should `intuition` partition the DPU's 4–8 hardware planes between `Screen`s and promoted windows (VQ31)? *Leaning:* each visible `Screen` gets at least one dedicated hardware DPU plane (plus the hardware cursor plane) so vertical screen dragging is always zero-copy in hardware; remaining DPU planes go to promoted video/GPU windows on the frontmost screen.

Raised in §16.3 and §16.8.

**VQ40 Per-plane colour space and HDR10 / wide-gamut scanout (`bm_ColorSpace`).**

Should `BitMap` carry `bm_ColorSpace` (`CS_SRGB`, `CS_BT709`, `CS_BT2020_PQ`) from day one so `MakeVPort` can program the DPU's hardware per-plane degamma/CSC/gamma blocks automatically? *Leaning:* yes; it defaults to `CS_SRGB` (`0`) and lets the DPU blend sRGB UI planes with HDR10/YUV planes during scanout.

Raised in §16.3 and §16.9.

**VQ41 Shader compilation pipeline (`tools/shaderc` fat `.gbin` vs. runtime micro-JIT in `gfx3d.library`).**

Should shaders ship as fat `.gbin` binaries compiled on the host by `tools/shaderc` (carrying Valhall/Immortalis, Adreno A6xx and SIMD bytecode sections) or should `gfx3d.library` include a small (~5–8k LoC) runtime translator from a restricted pointer-based SPIR-V subset to native GPU ISA? *Leaning:* build-time `tools/shaderc` `.gbin` bundles first for V5 (so the V5 triangle and compute demos need zero runtime compiler complexity on the board), then add a tiny pointer-first SPIR-V translator in user space once both G720/G610 and Adreno 690 are up.

Raised in §16.5 and §16.9.

## Audio and DSP

**VQ36 Hardware Audio DSP offload (`sky1_audss0` HiFi4, Q8B ADSP) vs. efficiency-core SIMD.**

Should V5/V6 drive the on-SoC audio DSPs, or run `audio` and `dsp.library` on an efficiency core (A520 on CIX, X60 on RV2) and keep the `dsp-*` queue lease optional? *Leaning:* run `audio` and `dsp.library` on CPU SIMD first (pinned to an A520 / cluster-1 core); keep the `DSP_PROVIDER_LEASE` hook in `dsp.library` so `sky1_audss0` can be bound later without changing client code.

Raised in §15.5 and §15.9.

**VQ37 Native tracker / module format support (`mod.library`).**

Should a compact tracker module player (`MOD` / `XM` or native `.pmod` using `F32` samples) ship in user space on top of `audio.library`'s retained `AudioVoice` API? *Leaning:* yes, a small bounds-checked `mod.library` (`< 1,500` lines of C23) as a system utility and conformance test of sample-accurate `Conductor` ticks.

Raised in §15.9.

## Coprocessors

**VQ16 Is the Hexagon CDSP in scope?**

It needs signed PAS firmware, its teardown can hang the SoC, and QNN only works with a faked SoC identity.

Raised in §5.3.

**VQ17 K1 GPU and AI instructions.**

Can we get usable vendor sources for the BXE-2-32? Is the 2 TOPS really in-core instructions?

Raised in §1.1 and §5.3.

## Plan

*(VQ19 decided; see the table above.)*
