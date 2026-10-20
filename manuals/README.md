# Planar Reference Manuals

Planar's version of the *Amiga ROM Kernel Reference Manuals*: the books a programmer writing for Planar reads. They are written in Markdown for now.

`docs/` and `manuals/` do different jobs:
- **`docs/`** is the design set. It says what Planar *should* be and why: proposals, decisions, open questions.
- **`manuals/`** says what Planar *is*: how to use what's built. A manual page never describes a call that doesn't exist yet without marking it **Planned** (see [STYLE.md](STYLE.md)).

When the two disagree, the code is right, the manual is a bug, and the design doc may be out of date.

## The volumes

The RKM was split into volumes by what the reader is doing. Planar keeps the split and adapts the subjects.

| Volume | RKM equivalent | What it covers | State |
|---|---|---|---|
| [**Libraries**](libraries/README.md) | *Libraries* | How to program Planar. Concepts, worked examples, rules. Exec first: tasks, signals, Areas, ports and messages. | Exec chapters started |
| [**Devices**](devices/README.md) | *Devices* | The I/O model (`IORequest`, `DoIO`, `SendIO`) and each device or driver server. | Outline only; nothing built |
| [**Autodocs**](autodocs/README.md) | *Includes and Autodocs* | One entry per call, in the RKM's NAME / SYNOPSIS / FUNCTION / … format. The reference, not the tutorial. | Every V2 call documented |
| [**Includes**](includes/README.md) | *Includes and Autodocs* | Each public header in `include/planar/`: its structures, constants and ABI promises. | Index and ABI rules |
| [**Hardware**](hardware/README.md) | *Hardware Reference Manual* | The machines Planar runs on, and the `arch_*`/`plat_*` contract a port implements. | Outline only |

A user-facing *DOS* volume (files, paths, the shell) and a *User Interface Style Guide* come when Planar has a filesystem and a display. The [Libraries](libraries/README.md) index lists the libraries expected before then.

## Reading order

New to Planar: [Libraries ch. 1](libraries/01-introduction.md), then chapters 2–4 in order. Each chapter ends with a list of the autodocs it relies on.

Looking up a call: [Autodocs](autodocs/README.md), alphabetical.

## Sources

Everything here is checked against:
- the public headers in `include/planar/`, which are the ABI;
- the kernel's handling of each call (`kernel/syscall.c` and what it calls) and `lib/exec/`;
- the conformance programs in `conformance/`. Most examples are taken from them, so they are known to run.

Each manual page names the version it was checked against at the top (for example *Checked against V2*). Before a release, a page whose version is older than the tree gets rechecked.
