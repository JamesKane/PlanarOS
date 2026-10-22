# dos: dos.library

*Checked against V6; the deadlines, step 7.*

The file calls, the loader, command lines and paths, and the console, in alphabetical order. The names are the RKM's (V6 step 6, [docs §17.4.4](../../docs/17-c-runtime-and-libraries.md)). The header is `planar/dos.h`; the code is `lib/dos/`, the archive `libdos.a`, linked into the commands (`cmd/`), hwdb, the GPU drivers and `dos*` conformance programs (`mk/user.mk`). None is a kernel call. The file calls each send one message to the `dos` server and wait (a trap when the server's asleep). `ReadArgs`, `AddPart`, `FilePart` and `PathPart` work on strings only and never ask the server. The volume is [DOS](../dos/README.md).

A path is `VOLUME:dir/file`, or under an assign: `SYS:`, the volume holding `EFI/planar`, and `C:`, `SYS:EFI/planar/C`. Case doesn't matter.

Errors are the kernel's (`planar/syscalls.h`), as the server gives them: `ERR_NOTFOUND` (no such volume or file), `ERR_RIGHTS` (a read-only volume, or a file not opened for writing), `ERR_EXISTS` (it's there already, or a directory isn't empty), `ERR_BUSY` (removing an open file), `ERR_FULL` (no room), `ERR_BADARG`, `ERR_BADHANDLE` (a file number not the caller's), `ERR_LIMIT` (64 files open in the server), and `ERR_PORT_DIED` if the server went. Every call to the server has a deadline, `DOS_DEADLINE_MS` (10 s, V6 step 7): past it the call gives `ERR_TIMEOUT` and hwdb is told dos is unresponsive (`ReportUnresponsive`), which restarts it; until dos answers the call it missed, the next ones give `ERR_BUSY`.

---

## dos/AddPart

**NAME**
AddPart, FilePart, PathPart -- make and take apart paths

**SYNOPSIS**
```c
#include <planar/dos.h>
bool AddPart(char *dirName, const char *fileName, unsigned size);
char *FilePart(const char *path);
char *PathPart(const char *path);
```

**FUNCTION**
`AddPart` puts `fileName` on the end of the path in `dirName`, in place, with a `/` between unless `dirName` ends in `:` or is empty:
- a name with its own volume replaces the path: `"SYS:a"` and `"C:x"` give `"C:x"`;
- a name starting `:` goes from the path's volume: `"SYS:a/b"` and `":x"` give `"SYS:x"`;
- each leading `/` is a step up, to the volume at most: `"SYS:a/b"` and `"/c"` give `"SYS:a/c"`.

`FilePart` is where a path's last part starts: `"b"` in `"SYS:a/b"`, `"a"` in `"SYS:a"`, `""` in `"SYS:"`. `PathPart` is where its directory ends: the `/` before the last part, or `FilePart`'s answer if there's none after the volume. A NUL written there leaves the directory.

**RESULT**
`AddPart`: true, or false if the result and its NUL wouldn't fit in `size` bytes, with `dirName` as it was. The others: a pointer into `path`.

**EXAMPLE**
From `cmd/type.c`, a name made whole from the Shell's current directory:
```c
SNPrintf(path, sizeof path, "%s", cwd);
if (!AddPart(path, *f, sizeof path)) {
    Printf(&c, "Type: too long: %s\n", *f);
    ...
}
```

**NOTES**
- The strings only: nothing asks the server, so a path made may name nothing.
- `AddPart`'s steps up are the Shell's `CD /` (`cmd/shell.c`).
- Tested by `conformance/argscheck.h`.

**BUGS**
None known.

**SEE ALSO**
[SplitArgs](#dossplitargs), [DOS ch. 1](../dos/01-files-and-programs.md)

---

## dos/CreateDir

**NAME**
CreateDir, DeletePath -- make a directory; delete a file or directory

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t CreateDir(struct Dos *dos, const char *path);
int64_t DeletePath(struct Dos *dos, const char *path);
```

**FUNCTION**
`CreateDir` makes an empty directory, whose parent must exist. `DeletePath` deletes a file, or a directory with nothing in it.

**RESULT**
0, or an error: `ERR_EXISTS` (it's there already; a directory not empty), `ERR_BUSY` (the file is open), `ERR_RIGHTS`, `ERR_NOTFOUND`.

**NOTES**
The RKM's are `CreateDir` and `DeleteFile`, which take a lock and a name. Planar has no locks: a path names it.

**BUGS**
None known.

---

## dos/ExamineNext

**NAME**
ExamineNext -- a directory's entries

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t ExamineNext(struct Dos *dos, uint32_t dir, uint32_t *cursor, struct DosEntry *entry);
```

**FUNCTION**
Gives the next entry of `dir`, a directory `OpenFile` opened. Start `*cursor` at 0. "." and ".." aren't given.

**RESULT**
0, or `ERR_NOTFOUND` after the last.

**EXAMPLE**
From `cmd/list.c`:
```c
while (ExamineNext(d, (uint32_t)f, &cursor, &e) == 0) {
    ...
    Printf(c, "  %s  %lu\n", e.de_Name, e.de_Size);
}
```

**BUGS**
None known.

---

## dos/ExaminePath

**NAME**
ExaminePath -- what's at a path

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t ExaminePath(struct Dos *dos, const char *path, struct DosEntry *entry);
```

**FUNCTION**
Fills in `entry` for the file or directory at `path` without opening it.

**RESULT**
0, or `ERR_NOTFOUND`.

**BUGS**
None known.

---

## dos/NextVolume

**NAME**
NextVolume -- the mounted volumes

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t NextVolume(struct Dos *dos, uint32_t index, struct DosEntry *entry);
```

**FUNCTION**
Describes volume `index` (from 0): `de_Name` its name, `de_Size` its free bytes, `de_Dir` 1 if it can be written.

**RESULT**
0, or `ERR_NOTFOUND` past the last.

**EXAMPLE**
From `cmd/info.c`:
```c
for (uint32_t i = 0; NextVolume(&d, i, &e) == 0; i++)
    ...
```

**NOTES**
Counting free space reads the whole FAT.

**BUGS**
None known.

---

## dos/OpenCon

**NAME**
OpenCon, WriteCon, PutStr, CloseCon -- the console

**SYNOPSIS**
```c
#include <planar/dos.h>
void OpenCon(struct Con *con, int64_t sigBit);
void WriteCon(struct Con *con, const char *text, uint32_t length);
void PutStr(struct Con *con, const char *string);
void CloseCon(struct Con *con);
```

**FUNCTION**
A program's output, as the Amiga's `Output()`. `OpenCon` connects to the display's console, "con.0". `WriteCon` sends text there, 64 bytes a message (`DRV_WRITE`); the display draws it and logs each line, so it reaches the serial console too. `PutStr` writes a string. `CloseCon` closes it. `Printf` formats onto it.

Without a console (no framebuffer, or the display gone), text goes to the kernel's log a line at a time, 159 bytes at most, and the rest of a line when `CloseCon` is called. If the console goes while it's being written (the display restarting), it's connected again once; after that, the log. A message the display hasn't answered in `CON_DEADLINE_MS` (1 s, V6 step 7) is reported to hwdb (`ReportUnresponsive("con.0")`) and treated as the console going.

**EXAMPLE**
From `cmd/echo.c`:
```c
OpenCon(&c, 16);
...
if (!a[1])
    PutStr(&c, "\n");
CloseCon(&c);
```

**NOTES**
- None returns an error: output always goes somewhere.
- The RKM's `PutStr` writes to `Output()`. Planar has no standard handles, so the console is named.

**BUGS**
A display that restarts twice loses the program to the log for good.

**SEE ALSO**
[Printf](#dosprintf), [Devices ch. 3](../devices/03-console.md)

---

## dos/OpenDos

**NAME**
OpenDos, OpenDosUntil, CloseDos -- connect to the dos server

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t OpenDos(struct Dos *dos, int64_t sigBit);
int64_t OpenDosUntil(struct Dos *dos, int64_t sigBit, uint64_t deadline);
void CloseDos(struct Dos *dos);
```

**FUNCTION**
Connects to "dos", with room for one message and a 64 KiB buffer (`DOS_MAX_IO`), and signal bit `sigBit`. Every file call names the connection. `OpenDosUntil` waits for dos to make its port, which it does once it has mounted the volumes, until `deadline` (`ConnectPortUntil`, V6 step 7): the Shell and dostest wait this way at boot.

**RESULT**
0, or `ERR_NOTFOUND` if `dos` isn't running (`OpenDos`), `ERR_TIMEOUT` if it wasn't by `deadline` (`OpenDosUntil`), `ERR_NOMEM`, or what `ConnectPort` gives.

**NOTES**
The Amiga's `OpenLibrary("dos.library")` opened the library; this opens a connection to the server, and a program may have several.

**BUGS**
None known.

---

## dos/OpenFile

**NAME**
OpenFile, CloseFile -- open and close a file or directory

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t OpenFile(struct Dos *dos, const char *path, uint32_t flags, struct DosEntry *entry);
int64_t CloseFile(struct Dos *dos, uint32_t file);
```

**FUNCTION**
Opens `path` and gives its number, for `ReadFile`, `WriteFile` and `ExamineNext`. `DOSF_WRITE` opens it for writing; with `DOSF_CREATE` too a file that isn't there is made, empty. `entry`, if not `nullptr`, gets what it is (`de_Name`, `de_Size`, `de_Dir`, `de_Date`, `de_Time`). A directory is opened to list it.

**RESULT**
The file's number (0 or more), or an error. `CloseFile` gives 0 or `ERR_BADHANDLE`.

**NOTES**
- A client's files close when its connection does.
- A made file gets an 8.3 name; a name that doesn't fit, or mixes case in a part, gives `ERR_BADARG` ([DOS ch. 1](../dos/01-files-and-programs.md)).
- The RKM's `Open` gives a `BPTR` filehandle with a position. Planar's is a number, and each read names its offset.

**BUGS**
Two numbers for one file each keep their own size: a file written through one isn't seen to grow through the other.

**SEE ALSO**
[ReadFile](#dosreadfile), [ExamineNext](#dosexaminenext)

---

## dos/Printf

**NAME**
Printf, VPrintf -- formatted text to the console

**SYNOPSIS**
```c
#include <planar/dos.h>
uint64_t Printf(struct Con *con, const char *format, ...);
uint64_t VPrintf(struct Con *con, const char *format, va_list args);
```

**FUNCTION**
Formats as `RawDoFmt` does ([exec](exec.md#execrawdofmt)): C's conversions but floating point, with `%d` an `int` and `%ld` 64 bits, C23's `%b`, and `%S` a `struct Str`. The bytes are gathered 64 at a time, a console message's worth, and sent with `WriteCon`.

**RESULT**
The bytes written.

**EXAMPLE**
From `cmd/info.c`:
```c
Printf(&c, "  %s: %lu MiB free, %s\n", e.de_Name, e.de_Size >> 20,
       e.de_Dir ? "read/write" : "read only");
```

**NOTES**
- Exec's `%ld` was 32 bits and `%d` 16; these are C's.
- The RKM's `Printf` writes to `Output()`; this one to the console named.
- Tested on the host over a fake console (`lib/dos/test.c`): what each message carries, and how many.

**BUGS**
None known.

**SEE ALSO**
[OpenCon](#dosopencon), [RawDoFmt](exec.md#execrawdofmt)

---

## dos/ReadArgs

**NAME**
ReadArgs, FreeArgs -- a command's arguments by template

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t ReadArgs(const char *argTemplate, int64_t *array, struct RDArgs *rdArgs);
void FreeArgs(struct RDArgs *rdArgs);
```

**FUNCTION**
Reads the line in `rdArgs->rda_Source` by an AmigaDOS template, as V36's did. The template is items separated by commas, each its names, aliases joined by `=`, and modifiers:

| | |
|---|---|
| `/A` | Required |
| `/K` | Only after its keyword |
| `/S` | A switch: `array[i]` is 1 if it's given |
| `/N` | A decimal number: `array[i]` points at an `int64_t` |
| `/M` | Many: `array[i]` points at a `nullptr`-ended array of the words. It takes every positional word from where it's reached, and gives back from its end what the `/A` items after it need, so `FROM/M/A,TO/A` reads `a b c d` as `FROM` a, b and c, and `TO` d. One a template. |
| `/F` | The rest of the line, as it stands |

A word that's one of an item's names is its keyword, and takes the next word, or what follows its `=` (`TO=x`), as its value. Any other word fills the first item not yet given that isn't `/K` or `/S`. Keywords are matched without case. Words are separated by spaces. A `"quoted"` word may hold spaces, with `*"` a quote, `*n` a newline, `*e` an escape and `**` a star, and is never a keyword. A value may be quoted too: `TO="a b"`.

`array[i]` is item `i`'s value: a string, a number's pointer, a switch's 1, or a `/M`'s array. An item not given is left as it was, so the caller sets its defaults first. The strings are copied into one `AllocVec` block, which `FreeArgs` gives back.

**INPUTS**
- `argTemplate` -- the template, 32 items at most.
- `array` -- an `int64_t` for each item.
- `rdArgs` -- the caller's, with `rda_Source` the line: `SplitArgs`'s arguments, or `nullptr` for none.

**RESULT**
0, or:
- `ERR_BADARG`, with `rda_Why` saying why, naming the item by its first name: "FILE is required", "too many arguments", "TO needs a value", "LINES needs a number", "ALL takes no value", "TO is given twice", "a quote isn't closed", "the command's template is wrong". `rda_Why` is `""` for a line that's only `?`: the caller shows the template.
- `ERR_NOMEM`.

After an error there's nothing to free, and `array` may be partly filled.

**EXAMPLE**
From `cmd/delete.c`:
```c
static const char TEMPLATE[] = "FILE/M/A,QUIET/S";
struct RDArgs ra = { 0 };
int64_t a[2] = { 0 };

SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
if (ReadArgs(TEMPLATE, a, &ra) != 0) {
    Printf(&c, "Delete: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
    ...
}
for (const char *const *f = (const char *const *)a[0]; *f; f++)
    ...
FreeArgs(&ra);
```

**NOTES**
- **Differs from AmigaDOS.** The RKM's reads `Input()` and returns a `struct RDArgs *`, allocating one if given none, with the error in `IoErr()`. Planar has no standard input or `IoErr()`: the line is named, the `struct RDArgs` is the caller's, and the error is returned, with its reason.
- No `/T`, which the template refuses. `?` doesn't prompt for a new line: there's no input to read it from.
- A number is decimal, with a sign, as AmigaDOS's are.
- `FreeArgs` of a `struct RDArgs` with nothing to free, or twice, is harmless.
- Tested by `conformance/argscheck.h`, in the guest and on the host with the sanitizers, and on random lines (`lib/dos/test.c`).

**BUGS**
None known.

**SEE ALSO**
[SplitArgs](#dossplitargs), [DOS ch. 2](../dos/02-shell.md#writing-a-command)

---

## dos/ReadFile

**NAME**
ReadFile, WriteFile -- move bytes

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t ReadFile(struct Dos *dos, uint32_t file, uint64_t offset, void *buffer, uint32_t length);
int64_t WriteFile(struct Dos *dos, uint32_t file, uint64_t offset, const void *buffer,
                  uint32_t length);
```

**FUNCTION**
Reads or writes `length` bytes at `offset`, 64 KiB a message, copied through the connection's buffer. A read stops at the end of the file. A write past it grows the file.

**RESULT**
The bytes moved, or an error. A read gives fewer than `length` at the end of the file; a write gives `ERR_FULL` if the volume filled.

**NOTES**
- A server copies: the data passes through the `dos` connection's heap and then the block connection's.
- The RKM's `Read` and `Write` move a filehandle's position. These name the offset.

**BUGS**
None known.

---

## dos/RunCommand

**NAME**
RunCommand -- load a program from a disk and start it

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t RunCommand(struct Dos *dos, const char *path, const char *args, int64_t sigBit,
                   uint32_t flags);
```

**FUNCTION**
Loads the program at `path` into a new Area and starts it with `CreateProcessImage`, named for the path's last part (`FilePart`). The program is an ELF static PIE for this machine, linked as the built-in ones are ([DOS ch. 1](../dos/01-files-and-programs.md#programs)). `args`, if not `nullptr`, is copied (up to `DOS_ARGS`, 1024 bytes) into a page past the image, and the program's `main` gets its address. `sigBit` and `flags` are `CreateProcessImage`'s.

**RESULT**
A process handle, or:
- `ERR_BADARG` -- not a 64-bit little-endian PIE for this machine, more than 16 program headers, or segments that don't fit (64 MiB at most, code ending on a page).
- Any error `OpenFile`, `ReadFile`, `AllocArea` or `CreateProcessImage` gives.

**EXAMPLE**
From `cmd/shell.c`:
```c
if ((p = RunCommand(&dos, path, args, SIG_CHILD, 0)) < 0) {
    say(p == ERR_NOTFOUND ? "Unknown command " : "Can't run ");
    ...
}
while (QueryProcess((Handle)p, &pi) == 0 && pi.pi_State < PROCS_EXITED)
    Wait(1ULL << SIG_CHILD);
```

**NOTES**
- The machine's ELF number comes from `arch/<isa>/user/elf.c`.
- The RKM's `RunCommand` runs a loaded segment on the caller's own process and waits. This one loads and starts a new process, and the caller waits if it wants to.

**BUGS**
None known.

**SEE ALSO**
[CreateProcessImage](exec.md#execcreateprocessimage), [SplitArgs](#dossplitargs)

---

## dos/SplitArgs

**NAME**
SplitArgs -- a command's line: its directory and its arguments

**SYNOPSIS**
```c
#include <planar/dos.h>
void SplitArgs(uint64_t arg, char *cwd, unsigned cwdSize, const char **args);
```

**FUNCTION**
Splits a command's `main` argument, the line the Shell gave `RunCommand`, into the current directory (up to its first newline) and the arguments after it. With no line, or none with a newline, `cwd` is "SYS:" and `args` the whole line.

**EXAMPLE**
From `cmd/echo.c`, the arguments straight to `ReadArgs`:
```c
SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
```

**NOTES**
Not the RKM's: AmigaDOS gave a command its current directory as a lock, and its arguments through `Input()`.

**BUGS**
None known.

**SEE ALSO**
[ReadArgs](#dosreadargs), [AddPart](#dosaddpart), [RunCommand](#dosruncommand)
