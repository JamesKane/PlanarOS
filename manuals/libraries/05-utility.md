# 5. utility.library: tags, hooks, strings, Span and Str

*Written, V6.*

utility.library holds what most programs want and the kernel has no reason to know: lists of tagged options, callbacks with their own data, case-blind comparison, formatting into a buffer, and safe reading of bytes whose layout comes from outside the program. Commodore's V36 utility.library had the first four. Planar adds `Span` and `Str`, after §17's argument that C's NUL-ended strings and bare pointers are the wrong tools for both ([docs §17.1](../../docs/17-c-runtime-and-libraries.md)).

Its functions are in `planar/utility.h` and `lib/utility`, in the archive `libutility.a`. A program links them if it calls them (chapter 1, *What every program links*). None keeps state, traps or allocates, so any task may call any of them at any time.

## Tag lists

A **tag list** is an array of `struct TagItem { Tag ti_Tag; uint64_t ti_Data; }`, ended by `TAG_DONE`. It's how a call takes options it may grow later: the caller names only the ones it sets, and a new option is a new tag, not a new argument. A program's own tags start at `TAG_USER`.

Four tags control the walk rather than carry data:

| Tag | Meaning |
|---|---|
| `TAG_DONE` (`TAG_END`) | The list ends. |
| `TAG_IGNORE` | Step over this item. A caller can switch an option off by overwriting its tag. |
| `TAG_SKIP` | Step over this item and the next `ti_Data` items. |
| `TAG_MORE` | The list goes on at `ti_Data`. A caller can add its own items before someone else's list without copying it. |

`NextTagItem` walks a list and returns only the items that carry data, following all four. `FindTagItem` and `GetTagData` walk with it, so they treat the control tags the same way.

### Reading options

```c
enum : Tag { OPT_Width = TAG_USER + 1, OPT_Height, OPT_Title };

static void open_thing(const struct TagItem *tags)
{
    uint64_t width = GetTagData(OPT_Width, 640, tags);
    const char *title = (const char *)GetTagData(OPT_Title, (uintptr_t)"untitled", tags);
    /* ... */
}

const struct TagItem tags[] = {
    { OPT_Width, 800 },
    { OPT_Title, (uintptr_t)"Shell" },
    { TAG_DONE, 0 },
};
open_thing(tags);
```

A function that takes many options walks the list once with `NextTagItem` and a `switch`, rather than calling `GetTagData` for each ([autodoc](../autodocs/utility.md#utilitynexttagitem)).

## Hooks

A **hook** is a callback that carries its own data: `struct Hook { h_Entry, h_SubEntry, h_Data }`. Whoever makes one sets `h_Entry` to the function and `h_Data` to what it needs, and gives the hook to a library. The library calls it with `CallHookPkt(hook, object, message)`, which calls `h_Entry(hook, object, message)`. The function finds its data through `hook->h_Data`, so no global is needed, and one function can serve many hooks.

What `object` and `message` are is the library's to say. Often the message is a structure whose first field says what's asked, so one hook can answer several questions.

## Case and comparison

`ToUpper` and `ToLower` change ASCII's letters and nothing else. `Stricmp` and `Strnicmp` compare C strings ignoring case. `StrSameNoCase` does the same for two `Str`s.

Commodore's changed ISO 8859-1's accented letters too. Planar's text is UTF-8, and Latin-1's mapping would change UTF-8's lead bytes, so a name with an accent in it would come back broken. Comparing names with accents as people would needs a locale, which Planar doesn't have yet.

## Formatting

`SNPrintf(buffer, size, format, ...)` formats into a buffer by `RawDoFmt` ([autodoc](../autodocs/exec.md#execrawdofmt)), with C's conversions but floating point. It always ends the text with a NUL if `size` isn't 0. It cuts what doesn't fit, and returns the length the whole would have had, so a result of `size` or more says the text was cut. `VSNPrintf` takes a `va_list`, for a function that formats for its caller:

```c
[[gnu::format(printf, 1, 2)]]
static inline void KPrintF(const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    VSNPrintf(line, sizeof line, fmt, ap);
    va_end(ap);
    KLog(line);
}
```

That's `conformance/log.h`, which every conformance program logs through. The attribute has the compiler check each call's arguments against its format.

## Span and Str

A **`Str`** is a string by its length: `{ const char *st_Data; uint64_t st_Len; }`. A **`Span`** is the same for bytes: `{ const uint8_t *sp_Data; uint64_t sp_Len; }`. Each is 16 bytes, passed and returned in two registers, so taking one costs no more than a pointer and a length.

Because a `Str` needs no NUL, a part of one is another `Str` pointing into it. Nothing is copied, and the memory can be read-only: a file in a shared Area, a firmware table, a font. `StrSlice`, `StrTrim` and `StrSplit` all return views of the `Str` they were given. `STR("literal")` makes one at compile time, and `StrFromC` makes one of a C string.

### Parsing a line

```c
struct Str line = STR("  timeout = 250\n"), key, value;
uint64_t ms;

if (StrSplit(StrTrim(line), '=', &key, &value) && StrSame(StrTrim(key), STR("timeout")) &&
    StrToU64(StrTrim(value), 10, &ms))
    use(ms);                                    /* 250 */
```

`StrToU64` reads all of its `Str` or fails. It refuses a byte that isn't a digit, and a number past 64 bits. C's `strtoull` stops at the first byte it can't read, and wraps a number too large.

### Reading a format from outside

A file's or a table's offsets and lengths come from the bytes themselves, and can be anything. Code that adds an offset to a pointer and then checks the result can be beaten by an offset large enough to wrap. The Span readers check every read against the span's end with `ckd_add`, before any pointer is formed, and refuse what doesn't fit:

```c
struct Span file = { data, size }, table;
uint32_t count, offset, length;

if (!SpanReadU16BE(file, 4, &count))            /* the header */
    return ERR_BADARG;
for (uint32_t i = 0; i < count; i++) {
    if (!SpanReadU32BE(file, 12 + 16 * i + 8, &offset) ||
        !SpanReadU32BE(file, 12 + 16 * i + 12, &length) ||
        !SpanSlice(file, offset, length, &table))
        return ERR_BADARG;                      /* past the end: refused */
    /* every read of 'table' is now checked against the table's own end */
}
```

The loads are of bytes, little- or big-endian as named. Any alignment is safe, and so is Device memory. A refused read leaves its result as it was.

`tools/span-fuzz` fuzzes the readers, `SpanSlice`, `StrToU64`, `StrSplit` and `StrTrim` under libFuzzer, ASan and UBSan, against a model whose bounds can't wrap. `make test-all` runs a million inputs. Changing the readers' check to a plain add, or letting it go one byte too far, fails it in under a second.

## Costs

Measured by `conformance/utility.c` under QEMU:

| | RV64 | AArch64 |
|---|---|---|
| `GetTagData` of the eighth tag | 50 ns | 55 ns |
| `SNPrintf` of a short line | 1.2 µs | 0.75 µs |
| `SpanReadU32BE` | 40 ns | 15 ns |

QEMU's figures say only how many instructions each takes. The boards' are measured when each is next on the bench.

## Rules

- **End every tag list with `TAG_DONE`.** A `TAG_MORE` to a list that loops back is walked forever.
- **Put pointers in `ti_Data` through `uintptr_t`.** It's 64 bits, as a pointer is.
- **Check `SNPrintf`'s result if the text must be whole.** A result of `size` or more means it was cut.
- **Keep the memory under a `Str` or `Span` alive while you use it.** They don't own what they point to.
- **Read what came from outside through a `Span`.** Don't add a file's offset to a pointer yourself.
- **`STR()` takes only a string literal.** Of a `char *` it gives the pointer's size.

## Differences from Exec

| Commodore | Planar |
|---|---|
| `ti_Data` is a 32-bit `ULONG` | `uint64_t`, so a pointer fits |
| `h_Entry` takes A0, A2 and A1, and C needs `HookEntry` as a stub | `h_Entry` is a C function, called directly |
| `ToUpper`, `Stricmp` know ISO 8859-1's case | ASCII's only, so UTF-8 passes through whole |
| No `SNPrintf` (V37) | `SNPrintf`, `VSNPrintf` over `RawDoFmt`, as C's `snprintf` returns |
| — | `Span`, `Str` and the checked readers |

## Functions used

[CallHookPkt](../autodocs/utility.md#utilitycallhookpkt),
[FindTagItem](../autodocs/utility.md#utilityfindtagitem),
[GetTagData](../autodocs/utility.md#utilitygettagdata),
[NextTagItem](../autodocs/utility.md#utilitynexttagitem),
[SNPrintf](../autodocs/utility.md#utilitysnprintf),
[SpanRead](../autodocs/utility.md#utilityspanread),
[SpanSlice](../autodocs/utility.md#utilityspanslice),
[StrFromC](../autodocs/utility.md#utilitystrfromc),
[StrHasPrefix](../autodocs/utility.md#utilitystrhasprefix),
[Stricmp](../autodocs/utility.md#utilitystricmp),
[StrSame](../autodocs/utility.md#utilitystrsame),
[StrSlice](../autodocs/utility.md#utilitystrslice),
[StrSplit](../autodocs/utility.md#utilitystrsplit),
[StrToU64](../autodocs/utility.md#utilitystrtou64),
[StrTrim](../autodocs/utility.md#utilitystrtrim),
[TagInArray](../autodocs/utility.md#utilitytaginarray),
[ToUpper](../autodocs/utility.md#utilitytoupper).
