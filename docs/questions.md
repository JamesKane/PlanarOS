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

No commitment until V1 has numbers from the RV2 and at least one Arm board. The kernel builds both layouts behind `arch_domain_*`. The global-VA layout comes straight from omega.

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

## Coprocessors

**VQ16 Is the Hexagon CDSP in scope?**

It needs signed PAS firmware, its teardown can hang the SoC, and QNN only works with a faked SoC identity.

Raised in §5.3.

**VQ17 K1 GPU and AI instructions.**

Can we get usable vendor sources for the BXE-2-32? Is the 2 TOPS really in-core instructions?

Raised in §1.1 and §5.3.

## Plan

*(VQ19 decided; see the table above.)*
