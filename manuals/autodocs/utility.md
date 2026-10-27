# utility: tags, hooks, strings, Span and Str

*Checked against V6.*

utility.library's functions, in alphabetical order: Commodore's V36–V39 tag lists, hooks, case-blind comparison and `SNPrintf`, and Planar's `Span` and `Str`, a pointer and a length with readers that check every offset. The header is `planar/utility.h`. They're library functions (`lib/utility`, in `libutility.a`), which a program links if it calls them. None traps, allocates or keeps state, so any task can call any of them at any time. [Libraries ch. 5](../libraries/05-utility.md) is the guide to them.

---

## utility/CallHookPkt

**NAME**
CallHookPkt -- call a hook

**SYNOPSIS**
```c
#include <planar/utility.h>
uint64_t CallHookPkt(struct Hook *hook, void *object, void *paramPacket);
```

**FUNCTION**
Calls `hook->h_Entry(hook, object, paramPacket)` and returns what it returns. A hook is a callback with its own data: whoever makes one sets `h_Entry` and, for its own use, `h_Data`. `h_SubEntry` is the hook's own, for a stub `h_Entry` that calls on to it.

**INPUTS**
- `hook` -- the hook.
- `object` -- what it acts on, as the caller and the hook's maker agree.
- `paramPacket` -- the message: often a structure whose first field says what's asked.

**RESULT**
What `h_Entry` returns; 0 if `hook` or `h_Entry` is `nullptr`.

**EXAMPLE**
```c
static uint64_t add(struct Hook *h, void *object, void *message)
{
    return (uintptr_t)h->h_Data + *(const uint64_t *)message;
}

struct Hook h = { add, nullptr, (void *)1000 };
uint64_t seven = 7;

CallHookPkt(&h, nullptr, &seven);       /* 1007 */
```

**NOTES**
- **Differs from Exec:** Commodore's `h_Entry` took its arguments in registers A0, A2 and A1, and C needed a stub (`HookEntry`) to put them on the stack. These are C calls: `h_Entry` is the C function itself.

**BUGS**
None known.

**SEE ALSO**
None.

---

## utility/FindTagItem

**NAME**
FindTagItem -- find a tag in a tag list

**SYNOPSIS**
```c
#include <planar/utility.h>
struct TagItem *FindTagItem(Tag tagValue, const struct TagItem *tagList);
```

**FUNCTION**
Walks `tagList` with `NextTagItem`, following `TAG_MORE` and stepping over `TAG_IGNORE` and `TAG_SKIP`, and returns the first item whose tag is `tagValue`.

**INPUTS**
- `tagValue` -- the tag. A control tag (`TAG_DONE`, `TAG_IGNORE`, `TAG_MORE`, `TAG_SKIP`) is never found.
- `tagList` -- the list, or `nullptr`.

**RESULT**
The item, or `nullptr` if it isn't in the list.

**NOTES**
- An item a `TAG_SKIP` skips, or after a list's `TAG_DONE`, isn't found.

**BUGS**
None known.

**SEE ALSO**
[GetTagData](#utilitygettagdata), [NextTagItem](#utilitynexttagitem)

---

## utility/GetTagData

**NAME**
GetTagData -- a tag's data, or a default

**SYNOPSIS**
```c
#include <planar/utility.h>
uint64_t GetTagData(Tag tagValue, uint64_t defaultVal, const struct TagItem *tagList);
```

**FUNCTION**
Returns the data of the first item in `tagList` with the tag `tagValue`, as `FindTagItem` finds it, or `defaultVal` if there's none.

**INPUTS**
- `tagValue` -- the tag.
- `defaultVal` -- what to return if it isn't there.
- `tagList` -- the list, or `nullptr`.

**RESULT**
The item's `ti_Data`, or `defaultVal`.

**EXAMPLE**
```c
enum : Tag { WA_Width = TAG_USER + 1, WA_Height };

uint64_t w = GetTagData(WA_Width, 640, tags);
```

**NOTES**
- **Differs from Exec:** `ti_Data` and the result are 64 bits, so a pointer fits as Commodore's 32-bit `ULONG` did.
- It costs about 50 ns for the eighth tag of a list on RV64 under QEMU, 55 ns on AArch64.

**BUGS**
None known.

**SEE ALSO**
[FindTagItem](#utilityfindtagitem)

---

## utility/NextTagItem

**NAME**
NextTagItem -- the next item of a tag list

**SYNOPSIS**
```c
#include <planar/utility.h>
struct TagItem *NextTagItem(const struct TagItem **tagListPtr);
```

**FUNCTION**
Returns the next item of the list `*tagListPtr` points into that isn't a control tag, and advances `*tagListPtr` past it. On the way it follows `TAG_MORE` (the list goes on at `ti_Data`), steps over a `TAG_IGNORE` item, and steps over a `TAG_SKIP` item and the `ti_Data` items after it. At `TAG_DONE`, or a `TAG_MORE` to `nullptr`, it sets `*tagListPtr` to `nullptr` and returns `nullptr`, as it does on every call after.

**INPUTS**
- `tagListPtr` -- where the walk is. Start it at the list.

**RESULT**
The item, or `nullptr` at the end.

**EXAMPLE**
```c
const struct TagItem *walk = tags;
struct TagItem *t;

while ((t = NextTagItem(&walk)))
    switch (t->ti_Tag) {
    case WA_Width: width = t->ti_Data; break;
    case WA_Height: height = t->ti_Data; break;
    }
```

**NOTES**
- Every function that reads a tag list walks it with this, so the control tags mean the same in all of them.
- The item is returned without `const`, as Commodore's was, so that a caller that owns the list can change it. The list itself is never written.

**BUGS**
A `TAG_MORE` chain that loops back on itself is walked forever.

**SEE ALSO**
[FindTagItem](#utilityfindtagitem)

---

## utility/SNPrintf

**NAME**
SNPrintf, VSNPrintf -- format into a buffer

**SYNOPSIS**
```c
#include <planar/utility.h>
int64_t SNPrintf(char *buffer, uint64_t bufferSize, const char *fmt, ...);
int64_t VSNPrintf(char *buffer, uint64_t bufferSize, const char *fmt, va_list args);
```

**FUNCTION**
Formats `fmt` with its arguments by `RawDoFmt` into `buffer`, cutting the text to `bufferSize - 1` bytes and ending it with a NUL. If `bufferSize` is 0, nothing is written. The conversions are `RawDoFmt`'s: C's without floating point, plus `%b` and `%S`.

**INPUTS**
- `buffer` -- where to write; may be `nullptr` if `bufferSize` is 0.
- `bufferSize` -- its size, the NUL's byte included.
- `fmt`, `...` or `args` -- the format and its arguments.

**RESULT**
The length of the whole text, without the NUL, as C's `snprintf` returns: if it's `bufferSize` or more, the text was cut.

**EXAMPLE**
```c
char line[160];

SNPrintf(line, sizeof line, "%s: %lu blocks, %#lx", name, n, addr);
KLog(line);
```

**NOTES**
- **Differs from Exec:** Commodore's V37 had no `SNPrintf`; it's what §17.4.3 adds over `RawDoFmt`. Its sizes are C's: `%d` an `int`, `%ld` 64 bits.
- `SNPrintf` isn't declared `[[gnu::format(printf, …)]]`, since the compiler takes `%S` for a wide string. A wrapper that is, such as `conformance/log.h`'s `KPrintF`, gets its format checked but can't take `%S`.
- A short line costs about 1.2 µs on RV64 under QEMU, 0.75 µs on AArch64: `RawDoFmt`'s, and a call per byte.

**BUGS**
None known.

**SEE ALSO**
[exec/RawDoFmt](exec.md#execrawdofmt)

---

## utility/SpanRead

**NAME**
SpanReadU8, SpanReadU16LE, SpanReadU32LE, SpanReadU64LE, SpanReadU16BE, SpanReadU32BE, SpanReadU64BE -- checked loads from a span

**SYNOPSIS**
```c
#include <planar/utility.h>
bool SpanReadU8(struct Span s, uint64_t offset, uint8_t *out);
bool SpanReadU16LE(struct Span s, uint64_t offset, uint16_t *out);
bool SpanReadU32LE(struct Span s, uint64_t offset, uint32_t *out);
bool SpanReadU64LE(struct Span s, uint64_t offset, uint64_t *out);
bool SpanReadU16BE(struct Span s, uint64_t offset, uint16_t *out);
bool SpanReadU32BE(struct Span s, uint64_t offset, uint32_t *out);
bool SpanReadU64BE(struct Span s, uint64_t offset, uint64_t *out);
```

**FUNCTION**
Loads the 1, 2, 4 or 8 bytes at `offset` in `s`, little-endian (`LE`) or big-endian (`BE`), into `*out`, if all of them are inside the span. The end is computed with `ckd_add`, so an offset however large can't wrap past the check.

**INPUTS**
- `s` -- the span.
- `offset` -- from its start, any value.
- `out` -- where the value goes.

**RESULT**
True and `*out` set; or false, with `*out` as it was, if any byte is past `s.sp_Len`.

**EXAMPLE**
A table's header, where the file says how long the table is:
```c
uint32_t tag, length;
struct Span table;

if (!SpanReadU32BE(file, at, &tag) || !SpanReadU32BE(file, at + 8, &length) ||
    !SpanSlice(file, at, length, &table))
    return ERR_BADARG;
```

**NOTES**
- The loads are of bytes: any alignment, and Device memory, are safe. The compiler makes one load and a byte swap of them where it can.
- `SpanReadU8` is Planar's addition to §17.4.3's list.
- Fuzzed by `tools/span-fuzz` under ASan and UBSan against a model whose bounds are 128-bit. A read of 4 bytes costs about 40 ns on RV64 under QEMU, 15 ns on AArch64.

**BUGS**
None known.

**SEE ALSO**
[SpanSlice](#utilityspanslice)

---

## utility/SpanSlice

**NAME**
SpanSlice -- part of a span, checked

**SYNOPSIS**
```c
#include <planar/utility.h>
bool SpanSlice(struct Span s, uint64_t offset, uint64_t len, struct Span *out);
```

**FUNCTION**
Sets `*out` to the `len` bytes at `offset` in `s`, if they're all inside it. Nothing is copied: `*out` points into `s`.

**INPUTS**
- `s` -- the span.
- `offset`, `len` -- the part, any values. `len` may be 0.
- `out` -- where the part goes.

**RESULT**
True and `*out` set; or false, with `*out` as it was, if `offset + len` is past `s.sp_Len` or would wrap.

**NOTES**
- Slicing a table out of a file, then reading it by offsets from 0, keeps every later read checked against the table's end, not just the file's.

**BUGS**
None known.

**SEE ALSO**
[SpanRead](#utilityspanread)

---

## utility/StrEqual

**NAME**
StrEqual -- two C strings the same

**SYNOPSIS**
```c
#include <planar/utility.h>
bool StrEqual(const char *a, const char *b);
```

**FUNCTION**
True if `a` and `b` hold the same bytes up to and including their NULs. Inline in the header, so a shared library or a driver can use it without linking `libutility.a`.

**INPUTS**
- `a`, `b` -- NUL-ended strings.

**RESULT**
`true` if they're the same, byte for byte; `Stricmp` is the case-blind comparison.

**NOTES**
- **Differs from Exec:** Commodore's had no such call; C's is `strcmp(a, b) == 0`.

**BUGS**
None known.

**SEE ALSO**
[Stricmp](#utilitystricmp)

---

## utility/StrFromC

**NAME**
StrFromC -- a Str of a C string

**SYNOPSIS**
```c
#include <planar/utility.h>
struct Str StrFromC(const char *s);
#define STR(lit) ((struct Str){ .st_Data = (lit), .st_Len = sizeof(lit) - 1 })
```

**FUNCTION**
Returns a `Str` of `s` up to its NUL. `STR()` does the same for a string literal, at compile time.

**INPUTS**
- `s` -- a NUL-ended string, or `nullptr`.

**RESULT**
The `Str`, the NUL not counted; the empty `Str` for `nullptr`.

**NOTES**
- `STR()` takes only a literal: of a `char *` it would give the pointer's size.

**BUGS**
None known.

**SEE ALSO**
[StrSlice](#utilitystrslice)

---

## utility/StrHasPrefix

**NAME**
StrHasPrefix -- whether a Str starts with another

**SYNOPSIS**
```c
#include <planar/utility.h>
bool StrHasPrefix(struct Str s, struct Str prefix);
```

**FUNCTION**
Whether the first bytes of `s` are `prefix`'s. Every `Str` has the empty one.

**RESULT**
True or false.

**BUGS**
None known.

**SEE ALSO**
[StrSame](#utilitystrsame)

---

## utility/Stricmp

**NAME**
Stricmp, Strnicmp -- compare C strings, case-blind

**SYNOPSIS**
```c
#include <planar/utility.h>
int Stricmp(const char *s1, const char *s2);
int Strnicmp(const char *s1, const char *s2, uint64_t n);
```

**FUNCTION**
Compares `s1` and `s2` byte by byte, each as `ToLower` makes it, to the first difference or NUL; `Strnicmp` no further than `n` bytes.

**INPUTS**
- `s1`, `s2` -- NUL-ended strings.
- `n` -- the most bytes to compare.

**RESULT**
Less than 0, 0, or more than 0, as `s1` sorts before, with, or after `s2`, the bytes taken as unsigned.

**NOTES**
- **Differs from Exec:** case is ASCII's, not ISO 8859-1's (`ToUpper`), so `"é"` and `"É"` in UTF-8 differ.

**BUGS**
None known.

**SEE ALSO**
[StrSame](#utilitystrsame), [ToUpper](#utilitytoupper)

---

## utility/StrSame

**NAME**
StrSame, StrSameNoCase -- whether two Strs are the same

**SYNOPSIS**
```c
#include <planar/utility.h>
bool StrSame(struct Str a, struct Str b);
bool StrSameNoCase(struct Str a, struct Str b);
```

**FUNCTION**
Whether `a` and `b` have the same length and bytes; `StrSameNoCase` with ASCII's case ignored.

**RESULT**
True or false.

**NOTES**
- A NUL in a `Str` is a byte like any other.

**BUGS**
None known.

**SEE ALSO**
[Stricmp](#utilitystricmp)

---

## utility/StrSlice

**NAME**
StrSlice -- part of a Str

**SYNOPSIS**
```c
#include <planar/utility.h>
struct Str StrSlice(struct Str s, uint64_t start, uint64_t end);
```

**FUNCTION**
Returns the bytes of `s` from `start` up to, not including, `end`. Both are clamped to `s`: an `end` past its length is its length, and a `start` past `end` gives an empty `Str` there.

**RESULT**
The slice, pointing into `s`.

**NOTES**
- Unlike `SpanSlice`, which a file's offsets feed and which refuses, this clamps, for a program's own indices.

**BUGS**
None known.

**SEE ALSO**
[SpanSlice](#utilityspanslice), [StrSplit](#utilitystrsplit)

---

## utility/StrSplit

**NAME**
StrSplit -- a Str either side of a separator

**SYNOPSIS**
```c
#include <planar/utility.h>
bool StrSplit(struct Str s, char sep, struct Str *head, struct Str *tail);
```

**FUNCTION**
Finds the first `sep` in `s`, and sets `*head` to what's before it and `*tail` to what's after it.

**INPUTS**
- `s` -- the Str.
- `sep` -- the byte to split at.
- `head`, `tail` -- where the parts go.

**RESULT**
True if `sep` was there. False if not, with `*head` all of `s` and `*tail` empty at its end.

**EXAMPLE**
A line of a configuration file:
```c
struct Str key, value;

if (StrSplit(StrTrim(line), '=', &key, &value)) {
    key = StrTrim(key);
    value = StrTrim(value);
}
```

**NOTES**
- Splitting `*tail` again walks a list: `"a,b,c"` at `','` gives `"a"`, then `"b"`, then `"c"` and false.

**BUGS**
None known.

**SEE ALSO**
[StrTrim](#utilitystrtrim)

---

## utility/StrToU64

**NAME**
StrToU64 -- a Str's number

**SYNOPSIS**
```c
#include <planar/utility.h>
bool StrToU64(struct Str s, unsigned base, uint64_t *out);
```

**FUNCTION**
Reads all of `s` as an unsigned number in `base`. Digits past 9 are letters, either case. Base 0 reads a prefix: `0x` hex, `0b` binary, `0o` octal, either case, and decimal otherwise. There's no sign, and no space is skipped.

**INPUTS**
- `s` -- the digits.
- `base` -- 2 to 36, or 0.
- `out` -- where the number goes.

**RESULT**
True and `*out` set; false, with `*out` as it was, if `s` is empty, a byte isn't a digit in `base`, the base is out of range, or the number is more than `UINT64_MAX` (checked with `ckd_mul` and `ckd_add`).

**NOTES**
- **Differs from C:** `strtoull` stops at the first byte it can't read and wraps a number too large. This refuses both, so `"12a"` and `"18446744073709551616"` are false.
- A leading 0 alone isn't octal: base 0 reads `"017"` as 17.

**BUGS**
None known.

---

## utility/StrTrim

**NAME**
StrTrim -- a Str without its blanks

**SYNOPSIS**
```c
#include <planar/utility.h>
struct Str StrTrim(struct Str s);
```

**FUNCTION**
Returns `s` without the spaces, tabs, CRs and LFs at either end.

**RESULT**
The trimmed `Str`, pointing into `s`; empty if it was all blank.

**BUGS**
None known.

**SEE ALSO**
[StrSplit](#utilitystrsplit)

---

## utility/TagInArray

**NAME**
TagInArray -- whether a tag is in an array of tags

**SYNOPSIS**
```c
#include <planar/utility.h>
bool TagInArray(Tag tagValue, const Tag *tagArray);
```

**FUNCTION**
Whether `tagValue` is one of the tags in `tagArray`, which ends with `TAG_DONE`. It's a set of tags, not a tag list: there are no control tags.

**INPUTS**
- `tagValue` -- the tag.
- `tagArray` -- the tags, ended with `TAG_DONE`, or `nullptr`.

**RESULT**
True or false.

**BUGS**
None known.

**SEE ALSO**
[FindTagItem](#utilityfindtagitem)

---

## utility/ToUpper

**NAME**
ToUpper, ToLower -- a character's case

**SYNOPSIS**
```c
#include <planar/utility.h>
int ToUpper(int c);
int ToLower(int c);
```

**FUNCTION**
`ToUpper` returns `c` in capitals if it's `a` to `z`, and `ToLower` in small letters if it's `A` to `Z`. Anything else is returned as it is.

**INPUTS**
- `c` -- a byte, 0 to 255.

**RESULT**
The byte, its case changed or not.

**NOTES**
- **Differs from Exec:** Commodore's changed ISO 8859-1's accented letters too. Planar's text is UTF-8, whose lead bytes (0xc3 for most of Latin-1's) that would change, so only ASCII's letters do.

**BUGS**
None known.

**SEE ALSO**
[Stricmp](#utilitystricmp)
