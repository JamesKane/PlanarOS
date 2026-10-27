# Open questions

Each entry gives the question, where it came up, and the current leaning or decision. A **★** marks a question that blocks V0 or V1.

## Decided

| # | Decision | Date |
|---|---|---|
| VQ1 | **Copy what we can from omega**: the build, the arch/plat contract, handles, signals, ports, lazy FP/vector state and the reclamation order. Staying compatible with omega afterwards isn't a goal. | 2026-10-19 |
| VQ2 | **The project is named Planar.** It started under the working name "vectra"; the earlier VectraOS corpus keeps that name. The VQ numbers stay as they are. | 2026-10-19 |
| VQ8 | **SMP from V1.** The scheduler and messaging run on all harts or cores from the first kernel milestone, with per-CPU run queues and placement by core class. | 2026-10-19 |
| VQ12 | **C23 plus assembly.** SIMD is expected in user code (apps, servers and libraries), and the kernel is built without it. The kernel stays small. Odin isn't part of the plan. | 2026-10-19 |
| VQ3 | **Identity SAS (VA = PA)**, decided by measurement on QEMU, the RV2 and the CIX P1. The global layout was deleted after V3. See [Memory](#memory) and [vq3-results.md](vq3-results.md). | 2026-10-20 |
| VQ4 | **The kernel stays in the upper half in both layouts**, with its direct map mapped global. Only user Areas move between VA = PA and the global VA window. See [v1-plan.md](v1-plan.md). (Only the identity layout remains, since VQ3.) | 2026-10-19 |
| VQ7 | **ACPI and FDT are both native sources.** The kernel parses static tables only. A user-mode `hwdb` builds one device graph. ACPICA runs unmodified in an isolated user-mode `acpid`. The Q8B merges ACPI with its firmware DTB. Board quirks are kept as data. Details in [09-hardware-description.md](09-hardware-description.md). | 2026-10-19 |
| VQ19 | **Board order: RV2, then CIX (Orange Pi 6 Plus), then RK3588, then Q8B.** The ACPI path gets proven on the second board. | 2026-10-19 |
| VQ14 | **FAT32 is required** for the EFI system partition and firmware interchange. The **native filesystem is a modern design**: journaling or copy-on-write metadata, extents and checksums are candidates. Snapshots and encryption are out of scope. The specific design is still open (VQ14a). | 2026-10-19 |
| VQ18 | **`<stdint.h>` types with canonical Commodore V36–V40 RKM `CamelCase` API names** across public libraries (`exec.library`, `utility.library`, `dos.library`). Struct interface tables (`IExec->`, `IUtility->`, `IDos->`) are adopted independently for C namespacing. Details in [17-c-runtime-and-libraries.md](17-c-runtime-and-libraries.md). | 2026-10-21 |
| VQ6 | **exec.library's interface (`IExec`) is handed to every process at its start, in the third argument register** (`a2`, `x2`): the first is `main`'s, the second hwdb's root lease. crt0 keeps it in the program's `IExec`. No fixed address: the identity layout can't promise one. See [v6-plan.md](v6-plan.md#4-interface-tables-and-the-shared-library-region). | 2026-10-22 |
| VQ22 | **Through the interface table, chosen at boot.** The kernel points exec.library's `CopyMem`, `CopyMemQuick` and `SetMem` at the vector unit's functions where the CPU has one (RVV 1.0; NEON, always), as it relocates the library. SVE2 waits for the kernel to save SVE's registers. | 2026-10-22 |
| VQ43 | **No PDP-11 `libc` in native code; Commodore V36–V40 library split + opt-in `lib/clib`.** Native programs use `exec.library` (`CreatePool`/`AllocPooled`/`AllocVec`/`RawDoFmt`), `utility.library` (`TagItem`, `Hook`, `Stricmp`, `SNPrintf`, 64-bit `Span`/`Str` slices), and `dos.library` (`ReadArgs`, `FilePart`, `Printf`). Standard C headers live in opt-in `include/clib/` (`lib/clib`) for third-party C ports (`acpid`), and LLVM `compiler-rt` 128-bit integer builtins are vendored into `third_party/compiler-rt/`. Details in [17-c-runtime-and-libraries.md](17-c-runtime-and-libraries.md). | 2026-10-21 |

## Memory

**VQ3 ★ Identity SAS (VA = PA) or global-VA SAS (Omega). Decide from measurements.**

**Decided (2026-10-20): identity SAS.** The data and the reasons are in [vq3-results.md](vq3-results.md), from QEMU, the RV2 and the CIX P1 (V3 step 6):
- Global costs 7–10% more per random touch on the A720, and up to 45–65% more on the A520. The Arm cores combine TLB entries only when VA and PA line up, which identity gives every page. On the X60 the layouts are within 3%.
- Identity leaves 38–60% longer free physical runs and uses 9–11% fewer page-table pages.
- Context switches cost the same in both.
- Identity's cost: under pressure about 0.15% of allocations fail with enough memory free, and the peak is about 5% lower. That is open work for V4.

The global layout was deleted after V3, in a commit of its own (v3-plan decision 3, 2026-10-20).

V1 measures:
- Largest free run per zone after a soak test that creates and destroys processes and grows and shrinks heaps.
- How often allocations fail even though enough total memory is free.
- Context switch and TLB miss cost in each layout.
- How much `IOF_LEND` and DMA setup the identity layout saves.
- How much code each layout needs in the loader and the allocator.

Raised in §3.1 and §8.

**VQ5 PMP and M-mode on RISC-V.**

Should we ever replace OpenSBI with our own M-mode monitor? *Leaning:* not before V13 (the PMP profile). The `pmp` back end stays in the contract.

Raised in §1.2 and §3.5.

**VQ6 How processes find ExecBase.**

**Decided (2026-10-22):** the third argument register at the start (above, *Decided*). The leaning was the first, but `main` has it, and hwdb's root lease the second.

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

**VQ18 Public type and function names.**

**Decided (2026-10-21):** `<stdint.h>` fixed-width types with canonical Commodore V36–V40 RKM `CamelCase` API functions across all public libraries, plus struct interface tables (`IExec->`, `IUtility->`, `IDos->`) for C namespacing. See [17-c-runtime-and-libraries.md](17-c-runtime-and-libraries.md).

Raised in §7.3 and §17.1.

**VQ43 C runtime and `libc` architecture.**

**Decided (2026-10-21):** Native Planar code uses Commodore-lineage `exec.library`, `utility.library` (upgraded with 64-bit `Span`/`Str` slices and checked binary readers), and `dos.library` rather than a Unix/PDP-11 `libc`. Standard C headers (`<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<stdio.h>`, `<stdbit.h>`) are strictly opt-in via `include/clib/` and `lib/clib` for third-party C code (`acpid`), and 128-bit integer builtins are vendored from LLVM `compiler-rt` into `third_party/compiler-rt/`. See [17-c-runtime-and-libraries.md](17-c-runtime-and-libraries.md).

Raised in §17.

**VQ13 Hardening features.**

Should BTI, PAC and MTE be used where they exist (A720)? *Leaning:* BTI and PAC on, MTE as a debug option.

Raised in §3.6.

**VQ22 SIMD in the shared library region.**

User code is expected to use SIMD, so the shared `mem*`/`str*` and codec helpers should use it too. Should one version be chosen per CPU at boot (RVV 1.0, NEON, SVE2), or should each library ship versions selected through its interface table?

**Decided (2026-10-22):** through the table, chosen as the kernel relocates the library at boot (above, *Decided*).

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

*Built (V4 step 3):* yes. The first restart comes after 10 ms, each next after twice as long, and hwdb gives up at 5 failures in 10 s (`servers/hwdb/server.h`).

Raised in §11.10.

**VQ24 Devices with no stop method.**

When a driver dies, something must stop its device before its DMA Areas are reused. What happens for a device with no reset line, no power domain of its own, no bus-master bit and no IOMMU? *Leaning:* its Areas stay quarantined for the rest of the boot, and the hwdb logs it. Each board's device list should be checked for such devices before V4.

*Built (V4 step 3), as leaned:* such a driver is restarted only if it held no DMA. Otherwise its lease stays stopping with the quarantine, hwdb logs it, and its devices fail.

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

**VQ42 AML that reads firmware memory.**

The CIX's `_STA` methods read a byte each from a firmware variable block at `0xfffe0000` (GNVA, 64 KiB the EFI map keeps reserved). The kernel refuses a lease over reserved memory, so acpid reads all ones, and the status of the 77 devices whose description reads it follows from that rather than from the firmware. Should hwdb grant acpid firmware memory its AML names, and on what terms? *Leaning:* a read-only lease over a range the EFI map marks ACPI NVS or reserved, for acpid only, with a write dropped and counted. A range the firmware protects (secure memory) must never be mapped, since a read there may abort, so the EFI map's type decides, not the address. Checked (2026-10-20): reading the page from FreeBSD through `/dev/mem` dropped FreeBSD into its debugger. Reading this range can bring down the operating system, so it stays refused until the EFI map's type for it is known and a safe way to read it is found.

Raised in V4 step 4 ([v4-plan](v4-plan.md)).

## Workbench and UI

**VQ31 Direct DPU plane promotion vs. full-screen composition.**

Should `intuition` assign visible top-level windows directly to hardware DPU overlay planes when within the plane budget, or compose into a single primary scanout buffer? *Leaning:* always use the hardware cursor plane for the pointer; compose windows into one primary scanout buffer first, then promote full-screen or GPU queue-lease windows to dedicated overlay planes.

Raised in §13.1 and §13.7.

**VQ32 Icon asset representation.**

Store icons in `.info` sidecar files (works on FAT32 and the native filesystem alike) with a compact vector command stream, or as pre-rasterised `ARGB8888` bitmaps? *Leaning:* `.info` sidecar files containing a tiny binary vector stream plus a cached `64×64` bitmap.

Raised in §13.4 and §13.7.

**Decided for V8 (2026-10-26):** neither yet. The Workbench draws the system icons as gfx paths in code; `.info` files come after V8 ([v8-plan](v8-plan.md) decision 3).

**VQ33 Window translucency (`Shell` backdrop blur).**

How should `intuition` implement the translucent `Shell` background before GPU composition is up on every board? *Leaning:* sample from a pre-blurred copy of the static procedural backdrop on CPU composition; optional GPU blur when `intuition` holds a queue lease.

Raised in §13.5 and §13.7.

**Decided for V8 (2026-10-26):** no translucency; the Shell's background is solid ([v8-plan](v8-plan.md) decision 11).

**VQ34 Base system font files (`System Sans` and `System Mono`).**

Which permissive open-source `.ttf`/`.otf` fonts should ship in the image via `#embed` for V4 (`Shell`) and V8 (`Workbench`)? *Leaning:* subsetted OFL builds of a geometric/humanist sans (Inter or IBM Plex Sans) and programmer monospace (JetBrains Mono or IBM Plex Mono) in the boot image, with unmodified full fonts in `tools/font-oracle/`.

Raised in §14.7.

**Decided (2026-10-26):** Inter for `System Sans` and JetBrains Mono for `System Mono`, OFL-1.1, static TrueType instances subset for the image ([v8-plan](v8-plan.md) decision 2).

**VQ35 Horizontal subpixel positioning (`1/4 px` bins) vs. whole-pixel glyph placement.**

Should `font.library` cache `4×` horizontal subpixel phases (`0.0`, `0.25`, `0.5`, `0.75 px`) at small UI sizes (`12–16 px`), or snap glyph origins to integer pixels? *Leaning:* `4×` horizontal subpixel bins for `System Sans` at `12–16 px`; integer origins for `System Mono` and sizes $\ge 18\text{ px}$.

Raised in §14.7.

**Decided (2026-10-26):** as it leaned ([v8-plan](v8-plan.md) step 3).

**VQ38 Compressed / tiled framebuffer modifiers (`MOD_AFBC_16x16`, `MOD_UBWC`) vs. linear-only surfaces in the Workbench (V7–V8).**

Should `AllocBitMap` default to `MOD_LINEAR` and reserve `MOD_AFBC_16x16` for explicit GPU-rendered scanout surfaces on CIX P1 (G720 + Linlon) and RK3588 (G610 + VOP2)? *Leaning:* yes; `MOD_LINEAR` (`ARGB8888` / `XRGB8888`) is the universal baseline for CPU SIMD and mixed surfaces, while `MOD_AFBC_16x16` is selected only when `BMF_GPU_ONLY | BMF_DISPLAYABLE` is requested on an AFBC-capable `friend_bitmap`.

Raised in §16.8.

**VQ39 Hardware screen dragging and multi-screen DPU plane budgeting.**

When multiple Intuition `Screen`s are visible at once (e.g. classic vertical screen dragging revealing a background screen at a different resolution or pixel format), how should `intuition` partition the DPU's 4–8 hardware planes between `Screen`s and promoted windows (VQ31)? *Leaning:* each visible `Screen` gets at least one dedicated hardware DPU plane (plus the hardware cursor plane) so vertical screen dragging is always zero-copy in hardware; remaining DPU planes go to promoted video/GPU windows on the frontmost screen.

Raised in §16.3 and §16.8.

**VQ40 Per-plane colour space and HDR10 / wide-gamut scanout (`bm_ColorSpace`).**

Should `BitMap` carry `bm_ColorSpace` (`CS_SRGB`, `CS_BT709`, `CS_BT2020_PQ`) from day one so `MakeVPort` can program the DPU's hardware per-plane degamma/CSC/gamma blocks automatically? *Leaning:* yes; it defaults to `CS_SRGB` (`0`) and lets the DPU blend sRGB UI planes with HDR10/YUV planes during scanout.

Raised in §16.3 and §16.9.

**VQ41 Shader compilation pipeline (`tools/shaderc` fat `.gbin` vs. runtime micro-JIT in `gfx3d.library`).**

Should shaders ship as fat `.gbin` binaries compiled on the host by `tools/shaderc` (carrying Valhall/Immortalis, Adreno A6xx and SIMD bytecode sections) or should `gfx3d.library` include a small (~5–8k LoC) runtime translator from a restricted pointer-based SPIR-V subset to native GPU ISA? *Leaning:* build-time `tools/shaderc` `.gbin` bundles first for V6 (so the V6 triangle and compute demos need zero runtime compiler complexity on the board), then add a tiny pointer-first SPIR-V translator in user space once both G720/G610 and Adreno 690 are up.

Raised in §16.5 and §16.9.

**VQ44 Embedded interface symbol tables vs. disk `.bmap` files for PlanarBASIC `LIBRARY`.**

Should Planar embed a compact function-name-to-slot table directly in each shared library's read-only RX image so `LIBRARY "gfx.library"` works without disk files, or load `.bmap` files from `LIBS:`? *Leaning:* embed the compact read-only symbol directory (`< 512` bytes per library, generated from `include/planar/*.h`) directly inside each shared library's RX image, and also accept `.bmap` alias files on disk.

Raised in [§18.8](18-planar-basic.md#188-open-questions).

**VQ45 Built-in `TRANSLATE$` and `SAY` speech synthesizer scope in PlanarBASIC.**

Should PlanarBASIC implement the rule-based English-to-phoneme translator (`TRANSLATE$`) and Klatt formant synthesizer (`SAY`) over `dsp.library`, or stub `SAY` until after V10? *Leaning:* after V10 (`audio` + `dsp.library`), implement the ~350-rule public-domain NRL English-to-phoneme table and a ~400-line cascade formant synthesizer over `dsp.library`'s `DSP_FilterBiquad`.

Raised in [§18.8](18-planar-basic.md#188-open-questions).

## Audio and DSP

**VQ36 Hardware Audio DSP offload (`sky1_audss0` HiFi4, Q8B ADSP) vs. efficiency-core SIMD.**

Should the audio phase (V10) drive the on-SoC audio DSPs, or run `audio` and `dsp.library` on an efficiency core (A520 on CIX, X60 on RV2) and keep the `dsp-*` queue lease optional? *Leaning:* run `audio` and `dsp.library` on CPU SIMD first (pinned to an A520 / cluster-1 core); keep the `DSP_PROVIDER_LEASE` hook in `dsp.library` so `sky1_audss0` can be bound later without changing client code.

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

**Partly answered (2026-10-20):** yes, the sources exist. Orange Pi's kernel (`orange-pi-6.6-ky`) has IMG's DDK 24.2 for core 36.29.52.182, a MIPS firmware processor and a server-queue provider ([v5-plan](v5-plan.md) step 6). Its firmware is mainline's open-source build for this core (`rogue_36.29.52.182_v1.fw`), so mainline's interface applies. Whether the 2 TOPS are in the cores is still open.

Raised in §1.1 and §5.3.

## Debugging

**VQ46 Hold or dump on fault?**

When a process faults with no debugger attached, should Planar keep killing it at once (today), hold it for a debugger to attach, or write a crash dump (registers, stacks, its Areas) to disk? *Leaning:* kill at once by default. Hold only processes marked hold-on-fault (`DBGF_HOLD`) or with a debugger attached. Crash dumps wait until post-mortem from the log alone proves too little.

Raised in [§19.6](19-debugging.md#196-the-debug-port).

**VQ47 Who may debug a process?**

*Leaning:* whoever holds the process's handle with `RIGHT_DEBUG`. The creator gets that right, and `init` grants the Shell's `Debug` command a lease to debug any process except core servers. Debugging `hwdb`, `power` or `dos` needs the serial transport, since stopping them can stop the Shell.

Raised in [§19.6](19-debugging.md#196-the-debug-port).

**VQ48 Breakpoints in the shared library region.**

Every process maps the library region, and a private copy of a page can't live at the same address (VA = PA). Should software breakpoints there be filtered by the kernel, with displaced instructions run out of line (uprobes-style), or should library code only take hardware breakpoints (typically 6 on Cortex-A cores, and often fewer triggers on RISC-V)? *Leaning:* out of line, since hardware breakpoints are too few once watchpoints compete for them.

Raised in [§19.7](19-debugging.md#197-breakpoints-stepping-and-watchpoints).

**VQ49 Where debug files live.**

Should the debug tables be appended to the image, or kept in separate files found by build-id? *Leaning:* separate files under a `DEBUG:` assign, named by build-id. The image carries only its build-id and the unwind table, so crash backtraces work without the files.

Raised in [§19.5](19-debugging.md#195-crash-records) and [§19.8](19-debugging.md#198-the-debugger-and-its-transports).

**VQ50 planar-c on Planar.**

On-board expression evaluation needs planar-c's parser, sema and lowering running on Planar. Port its library core (an `arena_planar.c` over `AllocArea`, plus a `dos.library` file layer), or evaluate expressions only from the host? *Leaning:* port it. The same port is also the first step towards compiling on the board.

Raised in [§19.8](19-debugging.md#198-the-debugger-and-its-transports).

**VQ51 Record and replay.**

Is a trace mode for rings and lent Areas, which would make per-process replay possible, worth its copies? *Leaning:* not before the debugger's steps 1 to 4 are done. Revisit with measurements of ring traffic.

Raised in [§19.10](19-debugging.md#1910-not-now-record-and-replay).

**VQ52 Unwinding through assembly.**

Should `.S` functions follow the frame-record convention through `FUNC_FRAME` and `FUNC_LEAF` macros, or carry hand-written unwind entries? *Leaning:* the macros. Assembly that can do neither (the trap entry, context switch) is marked a stopping point for the unwinder, and its registers come from the trap frame.

Raised in [§19.7](19-debugging.md#197-breakpoints-stepping-and-watchpoints).

**VQ53 When does planar-cc build PlanarOS?**

The tree is pinned to clang (LLVM 22, VQ12). planar-c already builds nine conformance programs that pass the boot test, and its M7 widens that to all of user space. Should the user-space build then switch to planar-cc by default, keeping clang as the oracle build? *Leaning:* yes, once M7's boot tests pass on QEMU and the boards. The debugger sees typed frames only in code planar-cc built (§19.11 step 2). The kernel, the `.S` files and inline `asm` stay clang's until planar-c has an assembler.

Raised in [§19.9](19-debugging.md#199-kernel-debugging) and [§8](08-roadmap.md#debugging).

## Plan

*(VQ19 decided; see the table above.)*
