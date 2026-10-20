# VQ3 measurements: identity vs. global layout

**Status: decided (2026-10-20): the identity layout.** The data is from QEMU, the Orange Pi RV2 and the CIX P1 (V3 step 6). The global layout is deleted after V3, in a commit of its own (v3-plan decision 3).

## How it's measured

`make vq3 [QEMU_MEM=…]` boots each QEMU platform with the `vq3` kernel argument. The kernel then runs the same pseudo-random workload against both layouts' allocators, one after the other, from the same free memory (`selftest_vq3` in `kernel/selftest.c`).

Workload:
- **Area sizes:** 70% are 1–8 pages, 25% are 9–128 pages, and 5% are 129–1,024 pages.
- **Stacks:** one Area in eight is a stack with a guard page.
- **Mix:** 6,000 operations, growing until 90% of free memory is held (at most 1,024 Areas), then freeing at random.
- **Page tables:** every 250 operations, 32 of the live Areas are mapped into a fresh domain and its page-table pages counted.

What it reports:
- **Failures:** allocations that failed, and how many of those failed *with enough memory free in total*. That second number is the identity layout's cost.
- **Time:** average time per allocation.
- **Peak:** the most memory held at once.
- **Largest free run:** the longest contiguous run of free physical pages at the end, before everything is freed. This is the global layout's cost, because scattered pages break up physical memory.
- **Page tables:** page-table pages per 32-Area process.

## Results (2026-10-19, QEMU 11.1, TCG, 4 CPUs)

| Memory | ISA | Layout | Failed (memory free) | Time per alloc | Peak held | Largest free run after | Tables per process |
|---|---|---|---|---|---|---|---|
| 256 MiB | rv64 | identity | 6 of 3,515 (6) | 166 µs | 220 MiB | 3 MiB | 17 |
| 256 MiB | rv64 | global | 0 of 3,511 | 177 µs | 232 MiB | 3 MiB | 20 |
| 256 MiB | aarch64 | identity | 7 of 3,514 (7) | 165 µs | 220 MiB | 3 MiB | 20 |
| 256 MiB | aarch64 | global | 0 of 3,511 | 148 µs | 231 MiB | 3 MiB | 20 |
| 1 GiB | rv64 | identity | 0 of 3,511 | 173 µs | 236 MiB | 745 MiB | 18 |
| 1 GiB | rv64 | global | 0 of 3,511 | 178 µs | 236 MiB | 263 MiB | 20 |
| 1 GiB | aarch64 | identity | 0 of 3,511 | 170 µs | 236 MiB | 742 MiB | 19 |
| 1 GiB | aarch64 | global | 0 of 3,511 | 154 µs | 236 MiB | 272 MiB | 21 |

At 1 GiB the 1,024-Area cap stops the workload near 236 MiB (about 25% occupancy), so the 90% target isn't reached. Since then, Area sizes scale with installed memory (one unit is `pmm_total / 65536` pages), so larger machines see the same pressure. QEMU at 256 MiB is unchanged by this, since its scale is 1.

### Orange Pi RV2 (2026-10-19, 8 GiB in two ranges, 8 × X60 at 1.6 GHz, sizes × 32)

| Run | Layout | Failed (memory free) | Time per alloc | Peak held | Largest free run after | Tables per process |
|---|---|---|---|---|---|---|
| 1 | identity | 6 of 3,514 (6) | 12.3 ms | 7,123 MiB | 211 MiB | 113 |
| 1 | global | 0 of 3,511 | 12.6 ms | 7,418 MiB | 117 MiB | 122 |
| 2 | identity | 6 of 3,514 (6) | 12.3 ms | 7,070 MiB | 180 MiB | 112 |
| 2 | global | 0 of 3,511 | 12.6 ms | 7,418 MiB | 117 MiB | 122 |

The board agrees with QEMU:
- Identity fails about 0.17% of allocations needlessly near 90% occupancy, and reaches a lower peak.
- Global leaves physical memory more fragmented (117 vs. 180–211 MiB largest free run).
- Identity used about 8% *fewer* page-table pages, even with the K1's two RAM ranges. That's the opposite of what §3 below expected.
- Allocation time is still mostly zero-filling. The kernel's `memset` works a byte at a time, so this number can't compare the allocators until the zero-fill is fixed.

### CIX P1 (2026-10-20, 32 GB in five ranges, 8 × A720 and 4 × A520, sizes × 127)

| Run | Layout | Failed (memory free) | Time per alloc | Peak held | Largest free run after | Tables per process |
|---|---|---|---|---|---|---|
| 1 | identity | 6 of 3,514 (6) | 729 µs | 27,996 MiB | 424 MiB | 353 |
| 1 | global | 0 of 3,479 | 1,006 µs | 29,714 MiB | 308 MiB | 397 |
| 2 | identity | 6 of 3,514 (6) | 730 µs | 27,996 MiB | 424 MiB | 353 |
| 2 | global | 0 of 3,479 | 1,007 µs | 29,714 MiB | 308 MiB | 397 |

### Orange Pi RV2 again (2026-10-20, 8 GiB, sizes × 32, `memset` no longer byte at a time)

| Run | Layout | Failed (memory free) | Time per alloc | Peak held | Largest free run after | Tables per process |
|---|---|---|---|---|---|---|
| 1 | identity | 5 of 3,513 (5) | 1,030 µs | 7,054 MiB | 187 MiB | 109 |
| 1 | global | 0 of 3,511 | 924 µs | 7,398 MiB | 117 MiB | 120 |
| 2 | identity | 5 of 3,513 (5) | 1,030 µs | 7,054 MiB | 187 MiB | 109 |
| 2 | global | 0 of 3,511 | 924 µs | 7,398 MiB | 117 MiB | 120 |

Allocation time is now the allocators' own, mostly. Global's page-at-a-time allocation costs about 44 ns more per page on the CIX, but is 10% faster on the RV2.

These RV2 runs needed a parser fix first. V2.5's hardening of the device-tree parser (review M14) had made a search stop at the end of the node it started from, so the kernel saw only the first of the RV2's two memory nodes. From that morning the RV2 ran on 2 of its 8 GiB. `V1.1 fdt` now checks that three nodes of one name are all found.

## Context switches and TLB misses (V3 step 6)

**Context switches cost the same in both layouts.** A domain switch is one TTBR0 or `satp` write with a 16-bit ASID and no flush: 64 cycles on the A720 and 118 on the X60, in either layout. A `DoMsg` round trip between two processes (`V3.5 costs`) differs between layouts by 2–5%, in no consistent direction: 1,540–1,575 cycles on the A720, 3,320–3,360 on the A520 and 3,110–3,290 on the X60.

**TLB misses** (`V3.6 tlb`, `conformance/tlbwalk.c`). A user program writes one word in every page of 64 or 192 Areas of 64 pages (16 or 48 MiB), in a random order, for 8 timed rounds after a warm-up round. The kernel counts user mode's TLB events on that CPU: PMUv3's common events on AArch64, and the SBI PMU's standard cache events on RISC-V. Per 1,000 touches:

| Core, size | Identity: cycles a touch | L2 TLB refills | Walks | Global: cycles a touch | L2 TLB refills | Walks |
|---|---|---|---|---|---|---|
| A720, 16 MiB | 27.7 | 0 | 0 | 30.4 (+10%) | 0 | 0 |
| A720, 48 MiB | 45.3–46.2 | 114 | 39 | 49.1–49.4 (+7%) | 381 | 135 |
| A520, 16 MiB | 162–166 | 75 | 163 | 236–277 (+45–65%) | 491 | 577 |
| A520, 48 MiB | 395 | 705 | 1,151 | 423 (+7%) | 824 | 1,207 |
| X60, 16 MiB | 425–437 | 1,001 DTLB misses | | 415 (−2.6%) | 1,001 DTLB misses | |
| X60, 48 MiB | 447–459 | 1,001 DTLB misses | | 438 (−2%) | 1,001 DTLB misses | |

Every touch misses the L1 TLB on all three cores, in both layouts. What differs is how often the next level misses too.

**Why identity misses less on Arm.** In a fresh boot the global layout's pages are physically contiguous too: 99% follow their neighbour. But only 6% have an address that agrees with their physical address below 64 KiB, which is chance (1 in 16). In the identity layout they all do. The A720 and A520 appear to combine neighbouring 4 KiB entries into one TLB entry when address and physical address are contiguous and aligned alike, so identity gets far more reach from the same TLB. The X60 shows no such effect.

The X60's counts are suspiciously round: 1,001 per 1,000 touches, and about 6,000 DTLB and 6,000 ITLB misses per 1,000 `DoMsg` round trips, in both layouts. Which of the hart's own events the firmware maps the SBI events to isn't known. Its timings are the firmer evidence.

**Across domain switches**, the Arm cores take almost no TLB refills per `DoMsg` (0–3 per 1,000 on the A720). ASIDs keep both processes' entries.

## Reading

1. **Identity fails needlessly under pressure.** At 90% occupancy, about 0.2% of allocations fail even though enough memory is free. Large Areas are the ones that fail, because each needs one run of free pages as long as itself. With low pressure (1 GiB), nothing fails.
2. **Global fragments physical memory.** At 25% occupancy the global layout leaves the longest free run about 2.8× shorter than identity does (≈270 vs. ≈745 MiB). Its scattered pages break up exactly what later contiguous allocations need: DMA buffers, scanout, `AREAF_CONTIG`, kernel stacks. A buddy-style allocator that hands out contiguous pages when it can would reduce this. That's untested.
3. **Page tables cost about the same.** Identity uses 8–15% fewer table pages, on QEMU and on the RV2 alike. I expected the K1's two RAM ranges (2 GiB at 0, 6 GiB at 4 GiB) to cost identity more tables; they didn't, at least with this workload.
4. **Allocation time favours neither.** Without byte-wise zero-filling, global is 38% slower on the CIX and 10% faster on the RV2.
5. **TLB misses favour identity on Arm.** Global costs 7–10% more per random touch on the A720, and up to 45–65% more on the A520 while the working set fits the TLB's combined reach. On the X60 the layouts are within 3%, global slightly ahead.
6. **Context switches cost the same.**
7. **Not measured:**
   - The DMA savings of identity, where pointers need no translation. That has to wait for drivers.
   - Real programs. The workload is synthetic.

## Decision (2026-10-20): identity

Identity wins on TLB cost on both Arm cores, on physical fragmentation (38–60% larger free runs), on page tables (9–11% fewer), and on the CIX's allocation time. It also keeps a pointer usable as a DMA address where there is no IOMMU. Global wins on failures: identity fails about 0.15% of allocations near 90% occupancy even with enough memory free, and peaks about 5% lower.

The failures are rare, fall on large Areas, and can be worked on in identity: reserving long runs for large Areas, or packing small ones away from them. Global's TLB cost falls on every big-memory Arm machine, and is largest on the newest little core.

**What follows:**
- The global layout and `layout=` are deleted after V3, in a commit of their own (v3-plan decision 3).
- Code above the protection back end may now assume VA = PA for user addresses (docs §3.1).
- Identity's needless failures under pressure are open work for V4, when servers allocate for real.
