# 1. Review of the vision paper

This document checks each section of the vision paper (*Design Specification for a Lightweight, Flat-Memory Operating System*) against the target hardware and the facts already measured. Each item gets one of these verdicts:

- **Keep**: sound as written.
- **Adapt**: the goal is right but the mechanism has to change.
- **Replace**: the mechanism doesn't work on these parts.
- **Gap**: the paper doesn't cover it.

Hardware facts come from `PlanOmega/docs/alpha/target.md` (RV2, measured) and `OS/AbyssBSD/docs/boards/*` (Q8B and Orange Pi 6 Plus, measured). Anything not yet verified on a board is marked *(verify)*.

## 1.1 Hardware profiles (paper §2)

| Paper claim | Finding | Verdict |
|---|---|---|
| The Q8B is ARMv8.4-A, with "Kryo Prime/Gold" cores | The cores are Cortex-X1C and Cortex-A78C (MIDR parts 0xd4c and 0xd4b), which are **Armv8.2-class**. No SVE. MTE_frac is reported. | Adapt: the arch baseline is v8.2 |
| Q8B "hardware mailboxes and SMEM" are used for CPU↔CPU events | Qualcomm's APCS IPC mailbox, SMEM and GLINK connect the application CPUs to **remote processors** (ADSP, CDSP, modem). Doorbells between CPU cores are GIC SGIs, and only EL1 can write them. | Replace: see §1.4 |
| The Q8B kernel owns the machine | The kernel runs at **EL1 under Qualcomm's EL2 hypervisor**, which owns stage 2 and polices SMMU programming. A wrong CBAR, SMR or GMU write resets the SoC instantly. | Gap: a hard constraint the paper doesn't mention |
| The RV2 has "16 GB" | Boards ship with 2, 4 or 8 GB. The tested 8 GB board has 2 GiB at PA 0 and 6 GiB at `0x1_0000_0000`. | Adapt |
| RV2 inter-core signalling uses ACLINT MSIP and SSIP | The RV2 has a SiFive-style **CLINT**, and `mtime` at `0xe400_bff8` reads as all ones from S-mode. No ACLINT SSWI device has been found *(verify)*. MSIP belongs to M-mode. Omega sends IPIs through the **SBI IPI extension**, which traps to OpenSBI. | Replace |
| The K1 has a "Daoyi AI engine" coprocessor with rings | The K1's 2 TOPS appears to come from **AI instructions inside the X60 cores** of cluster 0, using the 512 KiB TCM, rather than from a separate firmware-driven engine *(verify against SpacemiT docs)*. | Replace: see §5.5 |
| The K1 has an Imagination BXE GPU | It is a BXE-2-32 with firmware-based scheduling through kernel CCBs. The mainline device tree doesn't describe it, so a driver would need vendor sources *(verify)*. | Adapt |
| Orange Pi 5 Plus and 6 Plus | Not in the paper. **RK3588** (Mali-G610, CSF) and **CIX P1** (Immortalis-G720, CSF) are the only targets where the paper's Panthor model applies as written. | Gap: these boards fit the paper better than the ones it names |

## 1.2 Memory model (paper §3)

**Keep: the goal.** One address space, with protection independent of translation. The precedents are Opal, Mungi and 3DO Portfolio. Omega proved the approach on the RV2.

**Replace: "one immutable table, edit the AP bits on every switch" (§3.1).** This doesn't work, for four reasons:

1. **It is wrong on SMP.** One table walked by every core can't hold different permissions for the tasks running on different cores at the same moment. Opening task B's memory on core 1 opens it on core 0 too.
2. **It conflicts with large blocks.** The paper maps RAM with 1 GiB and 512 MiB blocks to keep TLB misses down, then sets permissions per process. A process's memory isn't 1 GiB-aligned, so the table has to be split to 4 KiB or 64 KiB pages wherever permissions differ. That gives up the TLB benefit the paper claims.
3. **It is slower than the alternative.** Every switch rewrites N descriptors, then runs DSB, a TLBI by VA for each range, DSB and ISB. Changing TTBR0/`satp` with ASIDs is a single register write, and TLB entries stay tagged (RV2 V1: 16-bit ASIDs, measured; global entries survive an ASID switch at 289 vs. 999 cycles for 32 pages). Switching TTBR does not flush caches, despite what the paper says.
4. **Stale TLB entries on other cores** would need broadcast invalidation on every switch.

**Replacement (proposed, §3):** an identity-mapped single address space. Each protection domain gets its own ASID-tagged translation table, and every table maps **VA = PA** for the regions that domain may touch. Pointers stay universal and physical, and translation exists only for protection and memory attributes. Shared library code and the system page live in one subtree that all domains point to. Large blocks are used where a whole region has the same rights, such as the kernel direct map and big DMA pools.

**Adapt: PMP (§3.2).**
- PMP CSRs exist only in **M-mode**. On the RV2, OpenSBI owns M-mode, and an S-mode kernel can't touch PMP. Using PMP means replacing OpenSBI with a Planar M-mode monitor, or running the kernel in M-mode (**VQ5**).
- The entry count is small: 16 or 64 by spec, and the X60's count hasn't been measured *(verify; M-mode probe)*. Firmware keeps several entries for itself. TOR uses two entries per region, and NAPOT needs power-of-two alignment.
- That leaves about 6–12 regions per domain. Code, data, stack, heap, the system region and every shared message arena all need one. One task with five shared arenas runs out.

PMP still matters, though. It is the right back end for a hobby CPU without an MMU, and for a small high-assurance "minimal" profile. §3.5 defines protection as a pluggable back end with two implementations: page tables (default) and region registers (PMP/MPU).

**Adapt: ePMP/W⊕X.** `mseccfg.MML` is about M-mode confining *itself*. Planar gets W^X from page-table permission bits on every target. ePMP is only relevant to the PMP back end.

**Keep: MAIR attribute table (§3.1).** The four attribute classes are right. On RISC-V, Svpbmt gives PMA, NC and IO (RV2 V13: present, no write-through, so `WRITETHROUGH` falls back to NC).

**Correction:** coherent masters must not get uncached buffers. On the Q8B, the CDSP snoops CPU caches, and uncached buffers make graphs fail. On CIX, FreeBSD mapping NPU DMA as Device memory cost 10×. The attribute is chosen per device (§5.2). "Rings are always non-cacheable" is not the rule.

**Gap: fragmentation and loading.** With VA = PA, a heap can't grow past its neighbour and nothing can be made "virtually contiguous". The paper doesn't cover:
- Loading relocatable executables (PIE ELF, relocated at load as in Omega)
- Stack sizing and guard gaps
- Physical fragmentation policy
- Address limits for 32-bit DMA (Q8B scanout must be contiguous and below 4 GB)

See §3.3–3.4 and **VQ3**.

## 1.3 Scheduling and vector state (paper §4)

**Keep: lazy vector save on RISC-V.** It is already done in Omega (`sstatus.FS`/`VS`). The S-mode kernel uses **`sstatus`**, not `mstatus`.

**Adapt: lazy NEON/SVE on ARM.** AArch64 has **no hardware dirty bit**. The lazy scheme there traps on first use (`CPACR_EL1.FPEN`), then saves on the next switch. NEON is part of the AArch64 baseline: compilers use it for `memcpy` and struct copies, User code is expected to use SIMD (VQ12, decided), so on ARM most tasks will dirty vector state anyway. The lazy scheme mainly benefits the kernel, which is built without SIMD, and tasks that are purely scalar. A first-use trap that rarely pays off may be worse than saving eagerly, so measure on the board before choosing (§7.4). On CIX, SVE2 makes the state larger (vector length to be probed).

**Gap: big.LITTLE and SMP.** Every target is heterogeneous (X1C/A78C, A76/A55, A720/A520, and two X60 clusters with separate L2s). The paper's "strictly prioritised queues" doesn't cover:
- Per-CPU run queues
- Placement, meaning which core class runs which priority band
- IPI cost
- DVFS (clocks and regulators via SCMI or the PMIC)

Omega v1 is single-core. Planar will be SMP from V1 (**VQ8**, decided).

## 1.4 IPC (paper §5)

**Keep:** Exec's model. Messages travel by reference and the receiver replies in place. Ports are named rendezvous points. Signals wake tasks.

**Replace: "any process `PutMsg`s straight into any port's lock-free ring" (§5.1).**
- If any sender can write into any receiver's ring, any process can corrupt any port. The paper's promise of "protecting the core from rogue processes" can't survive that.
- The receiver dereferences `mn_Payload`, so it needs read access to the *sender's* memory. With protection on, someone has to grant that access, and nobody does.
- `mn_ReplyPort` is a raw physical address, so a reply can be forged or aimed at any memory.

Planar uses Omega's answer: **connections with shared arenas**. A connection is created by `FindPort` or `OpenPort`. It sets up an arena that only the client and the server map, plus a pair of SPSC rings inside that arena. Messages and payloads must be in arenas that both sides can map. Reply ports are handles. Details in §4.

**Replace: "PutMsg writes the hardware doorbell" (§5.2).**
- User mode can't raise an IPI on either ISA. On AArch64, the `ICC_SGI1R_EL1` register belongs to EL1. On RISC-V, `MSIP` belongs to M-mode, and S-mode reaches it only through an SBI `ecall`.
- Waking a *blocked* task is a scheduler action, so the kernel has to be involved anyway.

Planar's fast path is futex-style. The sender enqueues into the shared ring in user mode. If the receiver's "waiting" flag is clear, nothing else happens. If it is set, one `Signal` call lets the kernel wake the receiver and send the IPI. Hardware mailboxes (Qualcomm APCS, CIX's SCMI mailboxes) are for **coprocessor** doorbells (§5), not for CPU-to-CPU IPC.

**Adapt: MPMC CAS ring.**
- Rings are per connection and SPSC: one client and one server per ring. That avoids CAS on the fast path and the ABA problems that come with MPMC rings.
- Fan-in to a server is handled by the server waiting on many rings with one signal mask.
- The paper doesn't say what happens when a ring is full. Planar's answer is backpressure: the sender blocks or gets `ERR_FULL` (**VQ10**).

## 1.5 Firmware as hardware (paper §6)

**Keep:** user-mode queues with doorbells and memory-resident sync objects, wherever the hardware supports them.

**Adapt: per device.** Command Stream Frontend (CSF) is an **Arm Mali** design.
- **RK3588 (G610) and CIX (G720):** CSF, so the model applies. The kernel side doesn't go away: someone still loads `mali_csffw.bin`, schedules CSGs when there are more groups than slots, programs the GPU MMU address spaces, and handles faults and resets. In Planar a **GPU server** does this, not the kernel.
- **Adreno 690:** not CSF. It has a privileged CP ringbuffer, GMU firmware, and a zap shader that TrustZone authenticates. User IBs are submitted through a privileged ring, so Planar puts that ring in a GPU server.
- **Hexagon CDSP (Q8B):** reached through FastRPC/GLINK. Firmware is signed and loaded through PAS. A PAS shutdown of a running DSP wedges the SoC. QNN on Linux only runs on the Q8B if the SoC claims to be a QCS6490.
- **CIX Zhouyi X2:** no firmware; jobs are register-driven.
- **RKNN:** register-driven, with an IOMMU per core.
- **K1:** in-core instructions, not a device.

`drm_panthor_sync_op` is the ioctl's descriptor for sync operations. The object in memory is the CSF sync object, a 64-bit sequence number with a status. The idea is right; only the name is wrong.

Planar generalises this as the **queue lease** (§5): one API for a ring, a doorbell and sync memory. It runs on hardware user queues where they exist and on a driver server's software queue everywhere else.

**Gap: IOMMU ownership.** "The OS programs the firmware's IOMMU" assumes the OS owns the IOMMU. That holds on CIX (SMMUv3) and RK3588 (an IOMMU per block). It doesn't hold on the Q8B, where the hypervisor polices the SMMU and UEFI's bypass banks must be left alone. The K1 has no IOMMU in front of the DPU *(verify)*. On those boards, any driver holding a DMA-capable lease is trusted (**VQ9**).

## 1.6 System API and the "known address" (paper §7)

**Keep:** a published, read-only system structure (ExecBase) plus a shared, global-mapped library region. Omega has the same thing as its shared region and system info page.

**Adapt: the "known physical address".** With VA = PA, no single physical address is guaranteed to be RAM on all four boards. The proposal is to pass ExecBase's address to every new process in its first argument register, which is Amiga's absolute address 4 in modern form (**VQ6**).

**Replace: "IORequests go to FilesystemPort with no trap".** This has the same problems as §1.4: writable foreign rings, and no way to wake a sleeping server. I/O uses connections and the futex-style fast path.

**Adapt: "SVC/ECALL flush pipelines and are costly".** The paper overstates the cost of a trap. A modern core takes one in tens to low hundreds of cycles, and Omega's whole `DoMsg` round trip (two traps plus two switches) is about 3.4k cycles on a 1.6 GHz in-order X60. The goal is to **keep traps cheap and few**, not to eliminate them. The three privileged categories the paper lists (protection changes, IRQ binding, coprocessor slot setup) match the shape of Omega's ~40-call kernel. To them Planar adds: block/wake, task and process life cycle, timers, and leases.

## 1.7 What the paper leaves out entirely

| Area | Where it's handled |
|---|---|
| Boot chains: OpenSBI/U-Boot, TF-A/U-Boot, UEFI at EL2, UEFI at EL1 under a hypervisor | §2.5, §6 |
| How the firmware describes hardware: ACPI (Q8B, CIX, K3, and most new SBCs) **and** FDT (RV2, RK3588) | **VQ7**, design session |
| Power, clocks, regulators, thermal (SCMI, RK806 PMIC, Qualcomm RPMh) | §2.4, servers |
| Storage (SD, eMMC, NVMe, UFS), filesystems | §2.4, **VQ14** (FAT32 plus a modern native FS) |
| Display scanout (K1 DPU, VOP2, Linlon/komeda, Qualcomm DPU) | §2.4 |
| USB, input, networking (RTL8125 over PCIe, EMAC, Qualcomm EMAC) | §2.4 |
| Loading firmware blobs (Mali CSF, Adreno SQE/GMU/zap, DSP) | §5 |
| Process life cycle, reclamation, crash containment | §2.3, from Omega |
| Debugging, conformance, measurement | §8 |
| Language and toolchain | §7 |
