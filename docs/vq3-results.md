# VQ3 measurements: identity vs. global layout

**Status: first data, from QEMU only. VQ3 stays open** until the same measurement has run on the RV2 and an Arm board (docs/v1-plan.md; the decision is due at V3).

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

At 1 GiB the 1,024-Area cap stops the workload near 236 MiB (about 25% occupancy), so the 90% target isn't reached.

## Reading

1. **Identity fails needlessly under pressure.** At 90% occupancy, about 0.2% of allocations fail even though enough memory is free. Large Areas are the ones that fail, because each needs one run of free pages as long as itself. With low pressure (1 GiB), nothing fails.
2. **Global fragments physical memory.** At 25% occupancy the global layout leaves the longest free run about 2.8× shorter than identity does (≈270 vs. ≈745 MiB). Its scattered pages break up exactly what later contiguous allocations need: DMA buffers, scanout, `AREAF_CONTIG`, kernel stacks. A buddy-style allocator that hands out contiguous pages when it can would reduce this. That's untested.
3. **Page tables cost about the same.** Identity uses 15% fewer table pages here, because QEMU's RAM is one small contiguous range. On boards whose RAM spans several GiB (the K1: 2 GiB at 0 and 6 GiB at 4 GiB), identity Areas spread over more of the address space, and that will cost more tables. Measure on the board.
4. **Allocation time can't be compared yet.** Zero-filling dominates it under TCG, and both layouts zero the same pages. The board will show the allocators' own costs.
5. **Not measured yet:**
   - TLB miss rates, and context-switch cost per layout (QEMU doesn't model TLBs).
   - The DMA savings of identity, where pointers need no translation. That has to wait for drivers.
   - Real programs. The workload is synthetic.

## What the board runs need to add

- The same `make vq3` workload on the RV2 (8 GiB, two RAM ranges) and on an Arm board.
- TLB-miss counters around a process touching its Areas, in each layout (omega's M4 instruments did this on the RV2).
- The page-table count on hardware with RAM spread across ranges.
