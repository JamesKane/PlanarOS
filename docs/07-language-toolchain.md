# 7. Language, toolchain and tree

## 7.1 Why C23, and candidates

### 7.1.1 Why C23 over newer systems languages

Planar turns a modern 64-bit board into a system one person can hold in their head. The language choice follows from that goal and from the architecture in [§0](00-vision.md), [§3](03-memory.md), [§4](04-ipc.md) and [§12](12-let-it-crash.md):

1. **Comprehension by the machine's owner.** Planar is written so that a user reading the kernel, a driver or `exec.library` can see straight through the source to the machine. C has the widest literacy of any systems language and the smallest gap between source text and emitted instructions. A struct is its memory layout; a function call is a `jal` or `bl`; nothing runs at scope exit or behind an operator. Newer systems languages (Rust, modern C++, Zig) ask the reader to master a second, compile-time language—trait bounds, lifetime elision rules, comptime metaprogramming or template instantiation—before they can read a page of driver code.
2. **Direct imperative control flow over functional patterns.** Operating system code is a sequence of state transitions on hardware registers, fixed tables and shared rings. It reads clearest when written as straight-line imperative steps: loops, `switch` statements, explicit bit operations, early returns on negative error codes (`ERR_BADARG`, `ERR_PORT_DIED`), and direct mutation of named fields. Newer systems dialects encourage functional idioms borrowed from application languages—monadic result chains (`.map().and_then()`), algebraic sum types, closures and iterator combinators. In low-level code those patterns hide control flow, obscure stack usage and branch costs, and turn a five-line state update into a puzzle of type transformations.
3. **Fault containment in hardware, not proof obligations in syntax.** The main pitch for newer systems languages is compile-time memory safety. Planar is deliberately unconcerned with proving memory safety in the compiler:
   - **No adversary model (P1, [§0.2](00-vision.md#02-principles)).** Planar is a single-user OS for a trusted machine, not a multi-tenant cloud host or a browser sandbox.
   - **Protection domains and "let it crash" ([§12](12-let-it-crash.md)).** Rather than paying a permanent tax in language complexity on every line of code to prevent pointer bugs statically, Planar contains faults at run time. Every server, driver and application runs in its own hardware protection domain (`kernel/process.c`). A bad pointer traps into `process_fault`, logs the faulting PC and address, kills only that process, reclaims every handle, Area and lease it owned (P6, [§11.4](11-driver-model.md#114-teardown-unbind-is-kill)), notifies peers with `ERR_PORT_DIED`, and lets its supervisor restart a fresh copy.
   - **Zero-copy shared memory fights borrow checkers.** Planar's defining mechanisms—universal pointers in a single address space (P2, [§3.1](03-memory.md#31-model)), lockless SPSC rings in shared `ConnArena`s ([§4.3](04-ipc.md#43-fast-path-no-trap-while-the-receiver-is-awake)), lending Areas across domains (`IOF_LEND`), and user-mode coprocessor rings ([§5](05-coprocessors.md))—all rely on aliased, cross-process mutable memory governed by protocol convention (**VQ20**) and hardware page permissions, not lexical lifetimes. In a borrow-checked language, nearly the entire IPC, Area and DMA surface would sit inside `unsafe` blocks over raw pointers anyway, paying for the language's friction while bypassing its guarantees.
   - **Small, static core.** The kernel itself is 10–15k lines of C ([§2.2](02-architecture.md#22-kernel-responsibilities)) over fixed-size tables and handle indices (`kernel/handle.c`), checked by clang's static analyser, host sanitizers (`plat/hosted`) and mutation-tested conformance suites (`conformance/`).
4. **C23 fixes C's gratuitous sharp edges without changing its nature.** Using C23 (§7.3) gives Planar typed constants (`constexpr`), `nullptr`, `typeof`, fixed-width underlying types on `enum`, compile-time layout checks (`static_assert`), checked integer arithmetic (`<stdckdint.h>`), `[[nodiscard]]` and `#embed`—all while keeping C's transparent imperative model and zero hidden runtime.

### 7.1.2 C23 vs. Odin

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
2. **The system ABI is a C ABI**, described once in `include/planar/`. Any other language binds to it later.
3. **SIMD is expected in user code**: apps, servers and libraries. **The kernel is built without SIMD** and is kept small.
4. **Namespacing in C.**
   - Public libraries are reached through *interface tables*: `IExec->PutMsg()`. This gives namespacing without name mangling, per-interface versioning, and the jump table §1.6 wants.
   - Public library functions follow Commodore's RKM 3rd Edition (Kickstart V36–V40) `CamelCase` names ([§17](17-c-runtime-and-libraries.md)).
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
- Public types are `<stdint.h>` types, paired with Commodore RKM `CamelCase` function names (**VQ18**, decided; [§17](17-c-runtime-and-libraries.md)). Omega's 32-bit Amiga typedefs (`ULONG`, `APTR`) are not used.
- Native Planar code uses `exec.library`, `utility.library`, and `dos.library` rather than a Unix `libc`; standard C library headers (`<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<stdio.h>`, `<stdbit.h>`) live in opt-in `include/clib/` (`lib/clib`) for third-party C ports ([§17](17-c-runtime-and-libraries.md)).

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
planar/
  Makefile            mk/<platform>.mk  mk/arch-<isa>.mk
  include/planar/     public C ABI (types.h, exec.h, syscalls.h, …)
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
