# Files and programs

*Checked against V6; long names made, and saved settings, V8 step 7.*

## What it's for

Reading and writing files, and starting programs that live on a disk rather than in the image. The `dos` server (`servers/dos`) serves the files; `dos.library` (`lib/dos`, `planar/dos.h`) is how a program asks it.

## Concepts

**Volumes.** At start `dos` opens every block port it finds ("nvme.0" to "nvme.3", "sd.0" to "sd.3"), reads each disk's GPT, and mounts each partition that holds FAT32. A volume is named by its label, or, unlabelled ("NO NAME") or when the label is taken already, by its device and partition: "nvme0p1", "sd0p1". Every volume can be written.

**Paths** are `VOLUME:dir/dir/file`. Case doesn't matter. Two names are assigned:
- `SYS:` is the volume holding `EFI/planar`, where `planar.efi` is: the EFI system partition Planar booted from.
- `C:` is `SYS:EFI/planar/C`, where commands are.

A command run from the Shell also gets the Shell's current directory, and `AddPart` makes a name relative to it: `"SYS:EFI"` and `"planar"` are `"SYS:EFI/planar"`, a name with its own volume stands alone, a name starting `:` goes from the volume's root, and each leading `/` is a step up. `FilePart` and `PathPart` take a path apart ([chapter 2](02-shell.md)). They work on the strings only.

**FAT32** (`servers/dos/fat.c`). FAT12 and FAT16 volumes aren't mounted.
- Long names are read and made. A name 8.3 holds, in one case ("hello.txt"), is made as one, keeping lower case by the flags Windows uses. Any other ("Hello.txt", "Env-Archive", "A Long Name.prefs") is made as a long name (V8 step 7) with a short alias, "BASIS~N.EXT", its first six characters and the first number not taken. A long name is printable ASCII, up to 255 characters, none of `"*/:<>?\|`, not ending in a dot or a space: anything else is `ERR_BADARG`. Lookups match the long name, in any case.
- Writes go straight to the disk, the FAT and its copies first, then the data, then the directory entry: a crash between leaves lost clusters at worst. The first write marks FSInfo's free count unknown, so whoever mounts next counts it.
- Every value read from the disk is checked before use; a damaged volume gives errors, not faults.

**Files** are opened by path and named by a number the server gives, which only the opener may use. They close when the opener's connection does. Data passes through a 64 KiB buffer in the connection.

## Using it

From `cmd/type.c`:

```c
struct Dos d;
struct DosEntry e;
int64_t f, n;
uint64_t off = 0;

OpenDos(&d, 17);
f = OpenFile(&d, "SYS:EFI/planar/hello.txt", 0, &e);
while ((n = ReadFile(&d, (uint32_t)f, off, buf, sizeof buf)) > 0)
    off += (uint64_t)n;
CloseFile(&d, (uint32_t)f);
```

Writing makes the file with `DOSF_CREATE`:
```c
f = OpenFile(&d, "SYS:EFI/planar/tmp/data.bin", DOSF_WRITE | DOSF_CREATE, nullptr);
WriteFile(&d, (uint32_t)f, 0, big, sizeof big);
```

## Programs

There's no ELF loader in the kernel (§2.2). `RunCommand` loads a program from a volume and starts it:

```c
int64_t p = RunCommand(&d, "C:list", "SYS:\nEFI/planar", SIG_CHILD, 0);
```

A program on a volume is the `.elf` the build already makes: a static PIE, linked at 0 with `arch/<isa>/user/user.ld`. Code and constants come first, then, on a page boundary, data and `.bss`. It relocates itself at start (`lib/rt/reloc.c`). `RunCommand`:
1. checks it's a 64-bit little-endian PIE for this machine;
2. allocates an Area big enough, plus a page, with `AREAF_EXEC`, and reads each loadable segment to its place;
3. copies the command line (up to 1024 bytes) into the extra page;
4. starts it with `CreateProcessImage`, whose `main` gets the command line's address. Under VQ3 the Area keeps its address in the child, so the pointer works there.

Commands are built from `cmd/NAME.c`, as ELF only, and put in `C:`: by `tools/mkdisk.py` on QEMU's disks, and by `tools/efi-boot.py`, next to `planar.efi`, on the CIX.

## Saved settings

A server's settings that Prefs saves are a small file each, `SYS:Prefs/Env-Archive/<server>.prefs` (`planar/prefs.h`, V8 step 7): a `PrefsHeader` (`"PRFS"`, version 1, a count) and that many `TagItem`s. `WritePrefs` writes one, making `SYS:Prefs` and its `Env-Archive` if they aren't there; `ReadPrefs` reads one back, the tags it holds ended by `TAG_END`. A reader takes the tags it knows and passes over the rest. From `servers/prefs/main.c`:

```c
struct TagItem t[] = {
    { PREFS_RepeatDelay, ip.ip_RepeatDelay }, { PREFS_RepeatRate, ip.ip_RepeatRate },
    { PREFS_PointerSpeed, ip.ip_PointerSpeed }, { PREFS_PointerAccel, ip.ip_PointerAccel },
    { TAG_END, 0 },
};
WritePrefs(&dos, "input", t);           /* SYS:Prefs/Env-Archive/input.prefs */
```

## Rules

- Long names are ASCII: a name past it is refused when made.
- Open a file once per writer: two opens of one file each keep their own idea of its size.
- A command reads its line with `SplitArgs`, and its arguments with `ReadArgs`; never assume `main`'s argument is 0.

## Differences from AmigaDOS

- No locks (`Lock`, `CurrentDir`): a path names a file, and the Shell passes its current directory as text.
- No `Open` returning a `BPTR` filehandle: a file is a number, and each read names its offset. The calls have the RKM's names where they do the RKM's job (`ReadArgs`, `AddPart`, `PutStr`, `Printf`, `CreateDir`), and names of their own where they don't (`OpenFile`, `ReadFile`, `ExaminePath`), so an Amiga programmer isn't misled.
- No `Input()` and `Output()`: a command names its console (`struct Con`), and `ReadArgs` names its line.
- Assigns are fixed: `SYS:`, `C:` and `S:` (`SYS:EFI/planar/S`, the scripts, V8 step 5). There's no `Assign` command yet.
- Settings are `ENVARC:`'s idea, at `SYS:Prefs/Env-Archive`, but tagged files of each server's rather than the Prefs editors' IFF ones, and there's no `ENV:` copy in RAM: each server holds its own in force.

## Functions used

[AddPart](../autodocs/dos.md#dosaddpart), [CreateDir](../autodocs/dos.md#doscreatedir), [ReadPrefs](../autodocs/dos.md#dosreadprefs), [WritePrefs](../autodocs/dos.md#doswriteprefs), [ExamineNext](../autodocs/dos.md#dosexaminenext), [ExaminePath](../autodocs/dos.md#dosexaminepath), [NextVolume](../autodocs/dos.md#dosnextvolume), [OpenDos](../autodocs/dos.md#dosopendos), [OpenFile](../autodocs/dos.md#dosopenfile), [ReadFile](../autodocs/dos.md#dosreadfile), [RunCommand](../autodocs/dos.md#dosruncommand), [SplitArgs](../autodocs/dos.md#dossplitargs), [CreateProcessImage](../autodocs/exec.md#execcreateprocessimage)
