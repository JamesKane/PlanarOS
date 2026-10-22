# compiler-rt's 128-bit builtins

The functions Clang calls for `__int128` and `_BitInt(128)` arithmetic it doesn't do inline: division and modulo on both ISAs (docs/17-c-runtime-and-libraries.md §17.4.1). Built into `libcompiler-rt.a`, which every user program links last.

- **Version 22.1.8**, from https://github.com/llvm/llvm-project at tag `llvmorg-22.1.8`, the pinned toolchain's (`mk/toolchain.mk`).
- **Unchanged.** Kept, from `compiler-rt/lib/builtins`: `udivmodti4.c`, `udivti3.c`, `umodti3.c`, `divti3.c`, `modti3.c`, `multi3.c`, and the headers they include (`int_lib.h`, `int_types.h`, `int_util.h`, `int_endianness.h`, `int_div_impl.inc`). Clang 22 inlines 128-bit multiplication on both ISAs; `__multi3` is kept for code built otherwise, and the archive only links what's called.
- **Built** by `mk/user.mk`, with the user programs' flags and its own warnings left alone.
- **Licence:** `Apache-2.0 WITH LLVM-exception` (each file's SPDX line, and `LICENSE.TXT`).
- **Tested** by `conformance/int128.c`, against the compiler's own constant folding.

To update: replace these files with the same ones from the pinned toolchain's tag, then run `make test-all`.
