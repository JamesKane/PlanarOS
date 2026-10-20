# Writing the manuals

The house rules for `manuals/`. They follow the RKM where it still works and the repo's own habits elsewhere.

## Status markers

Every call, structure, flag and behaviour is one of:

| Marker | Meaning |
|---|---|
| *(none)* | **Built.** It exists in the tree and a conformance test exercises it. This is the default and needs no marker. |
| **Built, untested** | It exists, but nothing in `conformance/` or `kernel/selftest.c` checks it. Say what's missing. |
| **Planned** | Designed (link the `docs/` section) but not built. Kept short: one paragraph and a link. Never a full autodoc. |
| **Differs from Exec** | Behaviour an Amiga programmer would get wrong. Always say what Exec did and what Planar does. |

A manual never promises what the design set only proposes. Write "planned" and link the design doc instead.

## Chapters (Libraries, Devices)

A chapter teaches. It is organised like an RKM chapter:
1. **What it's for**, in a paragraph.
2. **Concepts**: the objects involved and their lifetimes.
3. **Using it**: short examples, each taken from or checked against a conformance program.
4. **Rules**: the things that break if you ignore them. A bullet each.
5. **Differences from Exec**, where there are any.
6. **Functions used**: links to the autodocs.

## Autodocs

One `##` section per call, in alphabetical order within a file, in this form:

````markdown
## exec/AllocSignal

**NAME**
AllocSignal -- allocate a signal bit

**SYNOPSIS**
```c
#include <planar/exec.h>
int64_t AllocSignal(int64_t bit);
```

**FUNCTION**
What it does, in the present tense.

**INPUTS**
- `bit` -- …

**RESULT**
What it returns, every error it can return, and when.

**EXAMPLE**
Optional: a few lines of use, taken from a conformance program where possible.

**NOTES**
Anything a caller needs that isn't obvious. Differences from Exec go here.

**BUGS**
Known defects and gaps. "None known" if none.

**SEE ALSO**
Links to other autodocs and to the chapter.
````

Further rules:
- **RESULT lists every error the call can return**, read from the code, not from memory.
- Kernel calls say so: *"A kernel call."* Library functions say which library and whether they can trap.
- Name the file the behaviour lives in under NOTES when it's subtle, as `kernel/port.c`, so the next checker can find it.

## Prose

- The repo's style: short sentences, bullets for lists of three or more, plain words. British spelling as in `docs/`.
- Exec's names and capitalisation for public things (`AllocSignal`, `mn_Length`); the kernel's `subsystem_verb` names only when explaining internals.
- Code is C23, as the tree is: `nullptr`, `constexpr`, `bool`.
- Every chapter and autodoc file starts with *Checked against V*n*.* Update it when you recheck the page.

## Keeping it true

When a change to `include/planar/` or a kernel call lands, the same commit updates the autodoc. A new call gets its autodoc in the commit that adds it.
