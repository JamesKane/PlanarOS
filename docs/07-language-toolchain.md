# 7. Language, toolchain and tree

## 7.1 Candidates

| | C23 + asm | Odin | Note |
|---|---|---|---|
| Kernel / freestanding maturity | Excellent: clang 22, lld, every flag (`-ffixed-*`, `-fno-builtin`, `-mgeneral-regs-only`, kcfi, BTI/PAC) | Works with many workarounds (below) | |
| riscv64 + aarch64 freestanding | Both first class | aarch64 proven in VectraOS-Odin. **riscv64 never tried.** Odin has a riscv64 target *(verify freestanding support)* | RV2 is the first board |
| Namespaces | None. Prefixes by convention | Packages, `pkg.Name`; the main draw | |
| Control over SIMD codegen | Full (`-mgeneral-regs-only`, `-march` without V) | **Runtime and codegen emit SIMD everywhere** (441–939 vector instructions in a trivial kernel), which forced eager vector saves | Conflicts directly with lazy vector state (§1.3) |
| Inline asm | Full | `asm{}` undocumented, rejects `mrs`/`msr`, so every system register access costs a call | |
| Reproducible builds | Yes | No: nondeterministic checker, timestamps, unstable static numbering. VectraOS-Odin needed an IR canonicaliser | |
| Safety tooling | Analyzer, clang-tidy, kcfi, sanitizers in hosted builds | Bounds checks on, the others lost | |
| Typed addresses, bitfields, enumerated arrays | Weak (`typedef struct { uint64_t v; } paddr_t`) | Strong (`distinct`, `bit_set`, `bit_field`) | |

Source for the Odin rows: `OS/VectraOS-Odin/spikes/RESULTS.md`, `docs/adr/0001`, `0003`, `0004`, and `docs/CODING.md`. Those cover 86k lines and 365 commits of experience.

## 7.2 Decision

**Decided (VQ12, 2026-10-19).**

1. **The whole system is C23 plus `.S` assembly:** kernel, `arch/`, `plat/`, boot stubs, servers and libraries. Odin isn't part of the plan. §7.1 records why.
2. **The system ABI is a C ABI**, described once in `include/vectra/`. Any other language binds to it later.
3. **SIMD is expected in user code**: apps, servers and libraries. **The kernel is built without SIMD** and is kept small.
4. **Namespacing in C.**
   - Public libraries are reached through *interface tables*, in AmigaOS 4 style: `IExec->PutMsg()`. This gives namespacing without name mangling, per-interface versioning, and the jump table §1.6 wants.
   - Kernel internals use `subsystem_verb` prefixes (`port_put`, `area_share`), as in Omega.
5. **Vestra** stays a possible later language through the C ABI.

## 7.3 C23 baseline

- `-std=c23` with clang ≥ 19; the tree pins one LLVM version.
- Used:
  - `constexpr`, `nullptr` and `typeof`
  - `[[nodiscard]]` and `[[maybe_unused]]`
  - Enums with a fixed underlying type
  - `static_assert` for ABI layouts
  - `<stdckdint.h>` for size arithmetic
  - `#embed` for firmware blobs and user images, replacing Omega's generated `user_images.S`
  - `_BitInt` for register fields, used sparingly
- Distinct address types are single-member structs: `paddr_t`, `uaddr_t`.
- Public types are `<stdint.h>` types. Omega's Amiga typedefs (`ULONG`, `APTR`) are a question: they carry 32-bit meanings that read oddly on a 64-bit little-endian system (**VQ18**).

## 7.4 SIMD policy

**Decided.**

| Code | Flags | Why |
|---|---|---|
| Kernel | RV: `rv64imac_zba_zbb` (no F/D/V, as in Omega); A64: `-mgeneral-regs-only` | Small. Never touches user vector state except to save and restore it. |
| Servers, libraries, apps | Full ISA: RVV 1.0, NEON, SVE2 where present | SIMD is expected here |
| Shared `mem*`/`str*` and helpers | Picked per CPU at boot, or chosen through the interface table (**VQ22**) | |

How state is saved follows from this:
- RISC-V keeps the lazy save driven by `sstatus.VS`. It costs nothing to check, and scalar tasks and the kernel benefit.
- On AArch64 the choice between eager and lazy (a first-use trap) is made by measurement. With SIMD expected in user code, eager saving may well win.

## 7.5 Tree and build

**Decided: Omega's layout and Make.** PlanOmega Q16 was resolved on 2026-10-19 for Omega, and the same choice is adopted here.

```
vectra/
  Makefile            mk/<platform>.mk  mk/arch-<isa>.mk
  include/vectra/     public C ABI (types.h, exec.h, syscalls.h, …)
  kernel/             portable kernel + arch.h + plat.h
  arch/rv64/  arch/aarch64/{,efi/}  arch/host/
  plat/qemu-virt/  plat/ky-x1/  plat/rk3588/  plat/cix-p1/  plat/sc8280xp/  plat/hosted/
  drivers/            shared device code (uart8250, pl011, gicv3, plic, sdhci, nvme…)
  lib/  servers/      user-mode libraries and servers
  conformance/        serial-transcript tests, mutation-tested (Omega habit)
  tools/              check-portable.sh, serial-boot, mkimage helpers
  docs/               this design set + adr/
```

- `make PLATFORM=<p>`, `make run`, `make check`, `make sdcard`. Builds go in `build/<platform>/`.
- Decisions become ADRs in `docs/adr/`, a habit carried over from VectraOS-Odin, once questions in `questions.md` are settled.
