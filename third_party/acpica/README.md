# ACPICA

The ACPI Component Architecture, Intel's reference implementation of ACPI's AML interpreter and tables, which acpid runs in user mode (`servers/acpid/`, docs/09-hardware-description.md D3).

- **Version 20260930**, from https://github.com/acpica/acpica at commit `e85aa3b` ("release: Update version to 20260930"; the release isn't tagged).
- **Unchanged.** Kept: `source/include` (with `platform/`) and nine directories of `source/components`: dispatcher, events, executer, hardware, namespace, parser, resources, tables and utilities. Left out: the debugger, the disassembler, the compiler and the tools. These are the same files, byte for byte, that VectraOS-Odin ran.
- **Built** by `mk/acpica.mk`, for each ISA and the host, without `resources/rsdump.c` (the debugger's). acpid's `servers/acpid/acplanar.h` is force-included in place of `platform/acenv.h`'s list of systems.
- **Licence:** each file is `BSD-3-Clause OR GPL-2.0-only` (its SPDX line). Planar takes it under BSD-3-Clause (`LICENSE.BSD-3-Clause`); `LICENSE.GPL-2.0-only` is kept beside it, as Odin's copy had it.

To update: replace these directories with upstream's from a newer release, then run `make test-all`, and `make hwdb-test` with `HWDB_UPDATE=1` if acpid's reports change, reading the diff.
