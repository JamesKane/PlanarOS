# 3. Memory and protection

## 3.1 Model

**Proposed: an identity-mapped single address space (identity SAS).**

- All of the system lives in **one address space, and VA = PA**. A pointer is a physical address in every domain, and also to the kernel. On devices with no IOMMU, or an identity-mapped one, it is the address the device uses as well.
- A **protection domain** is a set of (region, rights) grants. The page-table back end turns that set into an ASID-tagged translation table that maps only the granted regions, each at its own physical address.
- A context switch is a single `satp` or `TTBR0_EL1` write. Switching TTBR does not flush caches. The RV2 measurement backs this up: switching ASID is cheap and global entries survive it.

How this differs from Omega:
- Omega keeps a **global VA allocator** that is separate from physical memory. Its layout is Areas at `0x1_0000`–`0x1F_FFFF_FFFF`, then the shared region, then lease windows.
- Vectra drops that layer, so the "where does this Area live" allocator is the physical page allocator.

| | Identity SAS (Vectra, proposed) | Global-VA SAS (Omega) |
|---|---|---|
| Pointer = DMA address | Yes, when there is no IOMMU or it is identity-mapped | No; translation needed |
| Area larger than the biggest free physical run | Impossible | Possible |
| Heap growth | Only if the neighbouring pages are free | Reserve VA, commit later |
| Null guard | Free on the RV2: PA 0–512 KiB belongs to OpenSBI and is never user-mapped. Elsewhere it's free if RAM doesn't start at 0. | Low 64 KiB unmapped |
| Fixed "known address" | No; ExecBase pointer is handed in at start (**VQ6**) | Possible |
| Fit with a PMP/MPU back end | Natural: PMP is physical | Needs a different design |
| Kernel view | The direct map is the address space | Separate upper-half map |

**Open (VQ3). Decided by measurement, not ahead of time.**

Identity SAS follows the paper most closely and makes devices simplest. Its cost is fragmentation. A hybrid could reserve one non-identity window for "virtual-contiguous" Areas, but every pointer into that window would be a non-DMA pointer, which brings back the distinction the design is trying to remove.

V1 builds both layouts behind `arch_domain_*`, with omega providing the global-VA layout. It then runs the soak test and cost probes listed under VQ3 in `questions.md` on the RV2 and an Arm board. Nothing above the protection back end and the physical allocator may assume VA = PA until that decision is made.

## 3.2 Kernel mapping

- The kernel runs with its own domain, which is the **whole of RAM identity-mapped** with large blocks (1 GiB and 2 MiB) plus device windows. In VA = PA, the kernel's view and the users' views share one numbering, so the kernel can use user pointers directly once it has checked them against the caller's grants.
- The kernel image is RX/RO/RW with W^X. It is mapped only in the kernel domain.
- On entry from user mode the kernel switches to its own domain. Alternatively, every domain's table carries supervisor-only kernel entries, as in Omega where kernel root entries are copied into every root. **VQ4** asks which.
  - Leaning: supervisor-only entries in every domain, since that avoids a TTBR write on each trap.
  - On AArch64, kernel data is mapped through TTBR0 with VA = PA, so PAN and UXN/PXN are needed to keep user code away from it.
  - With VA = PA there is no separate upper half, so the TTBR1 range is unused (**VQ4**).

## 3.3 Areas

**Proposed. Omega's API carries over unchanged.** An Area is a run of physical pages. It is page-aligned, normally physically contiguous, and lives until every process that maps it has dropped it.

| Flag | Meaning |
|---|---|
| `AREAF_CLEAR` | Zero-fill on allocation |
| `AREAF_DEVICE` | MMIO; device memory attributes |
| `AREAF_NC` | Normal non-cacheable memory, for non-coherent DMA |
| `AREAF_WC` | Write-combining (Arm Normal-NC, RISC-V Svpbmt NC); used for framebuffers |
| `AREAF_EXEC` | Executable; never writable at the same time |
| `AREAF_RWX` | Explicitly opt out of W^X (P1) |
| `AREAF_DMA32` | Must sit below 4 GiB (Q8B scanout, some PCIe devices) |
| `AREAF_LARGE` | Align so large mappings can be used |
| `AREAF_COHERENT` | The device snoops caches, so use the normal WB attribute (e.g. Q8B CDSP) |

`AllocMem` and memory pools are user-space libraries built on Areas, as in Omega.

## 3.4 Fragmentation and loading

These follow from VA = PA. All are **proposed**.

- **Executables:** PIE ELF, relocated at load time into one RX Area and one RW Area. There is no fork. Shared libraries live in the global RX region and are bound once at boot.
- **Stacks:** a fixed size, chosen at task creation, with a guard gap of unmapped pages. Under VA = PA a guard gap is a physical hole, so stacks come from a dedicated stack zone where guard pages can be reused as other stacks' guard pages.
- **Zoned physical allocator:**
  - Large Areas are allocated top-down in their own zone.
  - Small objects come from slabs in a separate zone.
  - DMA32 has its own reserved pool. The Q8B lesson: reserve it early.
  - Large GPU and NPU buffers come from CMA-style pools.
- **Heaps** grow by chaining Areas rather than extending one Area. The `alloc` library hides the chain.
- **Measure from the start.** `QuerySystem` reports largest-free-run per zone.

## 3.5 Protection back ends

**Proposed.** The `arch_domain_*` contract is written in terms of grants, so it doesn't assume page tables:

```c
int  arch_domain_create(struct domain *d);
int  arch_domain_grant(struct domain *d, paddr_t base, size_t len, uint32_t rights, uint32_t attr);
int  arch_domain_revoke(struct domain *d, paddr_t base, size_t len);
void arch_domain_switch(struct domain *d);   /* on context switch */
```

| Back end | Implementation | Use |
|---|---|---|
| `pt` (default) | Sv39 or Sv48 / VMSAv8-64 4 KiB granule. Identity entries; ASIDs; global bit for the shared region. | Every target board |
| `pmp` | Region registers loaded on switch. Grants beyond the hardware entry count are refused with `ERR_NO_REGIONS`, not emulated. | The paper's model. Needs M-mode (**VQ5**). Hobby CPUs without an MMU. |
| `mpu` | Armv8-R-style MPU | A possible future HellFire profile |

With the `pmp` back end, the system must fit within its region budget. In practice that means one arena per process rather than one per connection. It is a constrained profile, not the default.

## 3.6 Per-architecture notes

**RISC-V (RV2):**
- Sv39 gives 39-bit addresses, which covers the K1's PA range (top of RAM at `0x2_8000_0000` on the 8 GB board).
- ASIDs are 16 bits (measured).
- Svpbmt is present (PMA, NC, IO). Zicbom is present for cache maintenance on non-coherent DMA.
- `satp` writes need no fence when ASIDs aren't reused. On ASID rollover, use `sfence.vma` with the ASID.

**AArch64:**
- 4 KiB granule, 48-bit VA; IPS chosen per SoC.
- `TCR_EL1.A1 = 0`, so the ASID comes from TTBR0. Use 16-bit ASIDs if `ID_AA64MMFR0.ASIDBits` says so.
- nG on per-domain entries; global on the shared region.
- MAIR:

  | Index | Value | Type |
  |---|---|---|
  | 0 | `0x00` | Device-nGnRnE |
  | 1 | `0x04` | Device-nGnRE |
  | 2 | `0x44` | Normal-NC |
  | 3 | `0xFF` | Normal-WB |

- PAN on, and UAO where supported.
- CIX (Armv9.2): MTE and BTI may exist, to be probed. They are optional hardening (**VQ13**).
- Q8B:
  - Stage 2 belongs to the hypervisor, but stage 1 is ours.
  - IPA = PA as seen by EL1 *(verify identity stage 2; Linux behaviour suggests yes)*.

## 3.7 IOMMUs and DMA

| SoC | IOMMU | Vectra stance |
|---|---|---|
| RK3588 | Rockchip IOMMU per block (VOP, NPU, codecs); SMMUv3 instances off by default | Program identity mappings for the lease holder's grants. Device DMA is confined. |
| CIX P1 | SMMUv3 for PCIe and display/NPU; IORT RMRs; translating by default. GPU and VPU are not behind it. | Same identity approach. GPU confinement is through Mali's own MMU address spaces. |
| SC8280XP | MMU-500 ×2 plus SMMUv3 for PCIe, policed by the hypervisor. UEFI sets bypass SMRs. | **Don't touch** except to copy Linux's exact SMR values for the GPU. Drivers on this board are trusted. |
| K1 | None in front of the DPU *(verify)*; nothing in the DT | Drivers are trusted. Bounce buffers aren't needed because DMA is physical. |

**VQ9** asks whether "trusted driver" (no DMA confinement) is acceptable on the Q8B and K1. Leaning: yes, under P1, as long as the lease records that its DMA is unconfined.
