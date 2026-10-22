# 11. C's library

*Written, V6.*

Planar's own programs don't use C's library: they call Exec, utility.library and dos.library, whose calls say what they cost and return their errors. But code written for other systems expects `malloc`, `strlen` and `strtoul`, and rewriting it is a chance to break it. **`lib/clib`** is the part of C's library that such code needs and that Planar can give honestly, built on exec.library and utility.library ([docs §17.4.5](../../docs/17-c-runtime-and-libraries.md)). It's opt-in: a program gets it only by asking.

Its user today is acpid, which runs ACPICA, unchanged, over it.

## Using it

A program, or the third-party code in it, is built with C's headers from `include/clib`, and linked with `libclib.a` before `libutility.a` and `libexec.a`:

```make
$(BUILD)/user/servers/acpid/%.o: USER_CFLAGS += $(ACPICA_INC) -Iinclude/clib
```

`mk/user.mk` does this for acpid, and for any conformance program whose name starts `clib`. `#include <string.h>` then finds `include/clib/string.h`. Nothing else in the tree sees those headers: a Planar program that includes `<string.h>` without them fails to build, which is the point.

The headers are C11 as well as C23, since third-party code is often built as C11 (ACPICA is, with `-std=gnu11`).

## What's there

| Header | Contents |
|---|---|
| `<stdlib.h>` | `malloc`, `calloc`, `realloc`, `free`, `memalignment`; `strtol`, `strtoll`, `strtoul`, `strtoull`; `qsort`, `bsearch`; `abs`, `labs`, `llabs`; `exit`, `_Exit`, `abort` |
| `<string.h>` | `memcpy`, `memmove`, `memset`, `memcmp` (lib/rt's, which every program has), `memset_explicit`, `memchr`; `strlen`, `strnlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strchr`, `strrchr`, `strstr` |
| `<ctype.h>` | `isalnum` to `isxdigit`, `toupper`, `tolower`: the C locale's |
| `<stdio.h>` | `snprintf`, `vsnprintf`, `EOF`, and nothing else |
| `<inttypes.h>` | `PRId64` and the rest of the `PRI` macros; `strtoimax`, `strtoumax`, `imaxabs` |
| `<stdbit.h>` | C23's `stdc_leading_zeros` to `stdc_bit_ceil`, typed and type-generic |
| `<errno.h>` | `errno`, `ENOMEM`, `EINVAL`, `EDOM`, `ERANGE`, `EILSEQ` |
| `<assert.h>` | `assert`, and `static_assert` for C11 |

`<stddef.h>`, `<stdint.h>`, `<stdarg.h>`, `<stdbool.h>`, `<limits.h>` and `<stdckdint.h>` are the compiler's, as for every program.

## Memory

`malloc` and its family are exec.library's `AllocVec` and `FreeVec` (chapter 3): blocks from the pool every task of the process shares, or an Area of their own past the pool's threshold of 16 KiB. So a block from `malloc` can be given to `FreeVec`, and one from `AllocVec` to `free`.

- Every block is aligned to 16, as `max_align_t` is.
- `malloc(0)` is a block of its own, not `nullptr`, so code that tests for `nullptr` doesn't take it for a failure.
- `calloc` checks `n * size` with `ckd_mul`, and is `nullptr` if it overflows.
- `realloc` keeps a block that's big enough already, so a shrunk block keeps its room. Otherwise it copies into a new one.
- A failure is `nullptr`, with `errno` set to `ENOMEM`.

## Numbers

The `strtol` family reads as C23 says: spaces, a sign, then digits, with `0x` for base 16, `0b` for base 2 (C23's, which older libraries don't take), and base 0 taking either, a leading `0` for octal, or else decimal. Past the type's range the result is its limit, with `errno` set to `ERANGE`. A base that isn't 0 or 2 to 36 is `EINVAL`. With no digits, `*end` is the string itself.

To read a number that must be the whole of a string, utility.library's `StrToU64` is the better call: it refuses anything else (chapter 5).

## errno

`errno` is `thread_local`: each task has its own, in its thread block (chapter 2). Only lib/clib sets it. Planar's own calls return their errors and never touch it.

## Ending

`exit` and `_Exit` are `ExitProcess`, which ends every task of the process. There's no `atexit`, and nothing is flushed, since there's nothing buffered. `abort` logs `abort() called` and exits with `EXIT_FAILURE`. A failed `assert` logs the expression, the file and line, and the function, then does the same.

## Costs

Measured by `conformance/clib.c`, under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `malloc` and `free` of 64 bytes | 280 ns | 110 ns |
| `strtoul` of 10 digits | 90 ns | 85 ns |
| `qsort` of 1000 `int`s | 1.0 ms | 0.29 ms |

`malloc` costs what `AllocVec` does, and a call more. QEMU's figures say only how many instructions each takes. The boards' are measured when each is next on the bench.

## Rules

- **Ask for it only for code that wasn't written for Planar.** Planar's own code calls Exec and utility.library: `AllocVec`, `SNPrintf`, `StrToU64`, the `Span` readers.
- **Check `malloc`'s result.** It's `nullptr` when the process's memory or its handles run out.
- **Don't write past a block.** Nothing guards its end: the next block's header is there.
- **Take `errno` straight after the call that set it**, and set it to 0 before a `strtol` whose range you need to know.
- **Don't take the address of a `<ctype.h>` function.** They're inline.

## Differences from C

| C | Planar's lib/clib |
|---|---|
| `<stdio.h>`'s `FILE`, `printf`, `fopen` | None: a program writes through dos.library ([DOS manual](../dos/README.md)) |
| `printf`'s conversions | `RawDoFmt`'s ([autodoc](../autodocs/exec.md#execrawdofmt)): C's, but floating point |
| `<ctype.h>` by locale | The C locale's only. A byte above 0x7f is in no class |
| `<ctype.h>`'s functions are also functions | Inline only |
| `qsort` usually a quicksort | A heapsort: n log n at worst, no recursion, no allocation. Not stable, which C doesn't ask |
| `malloc(0)` may be `nullptr` | Always a block |
| `atexit`, `getenv`, `signal`, `time`, `setjmp` | Not there |
| `<math.h>`, floating point | Not there |

## Functions used

[AllocVec](../autodocs/exec.md#execallocvec),
[ExitProcess](../autodocs/exec.md#execexitprocess),
[FreeVec](../autodocs/exec.md#execfreevec),
[KLog](../autodocs/exec.md#execklog),
[RawDoFmt](../autodocs/exec.md#execrawdofmt),
[VSNPrintf](../autodocs/utility.md#utilitysnprintf).
