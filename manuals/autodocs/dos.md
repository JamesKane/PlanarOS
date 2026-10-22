# dos: dos.library

*Checked against V5.*

The file calls, the loader and the console, in alphabetical order. The header is `planar/dos.h`; the code is `lib/dos/dos.c`, linked into the commands (`cmd/`), hwdb, and `dos*` conformance programs (`mk/user.mk`). None is a kernel call. The file calls each send one message to the `dos` server and wait (a trap when the server's asleep). The volume is [DOS](../dos/README.md).

A path is `VOLUME:dir/file`, or under an assign: `SYS:`, the volume holding `EFI/planar`, and `C:`, `SYS:EFI/planar/C`. Case doesn't matter.

Errors are the kernel's (`planar/syscalls.h`), as the server gives them: `ERR_NOTFOUND` (no such volume or file), `ERR_RIGHTS` (a read-only volume, or a file not opened for writing), `ERR_EXISTS` (it's there already, or a directory isn't empty), `ERR_BUSY` (removing an open file), `ERR_FULL` (no room), `ERR_BADARG`, `ERR_BADHANDLE` (a file number not the caller's), `ERR_LIMIT` (64 files open in the server), and `ERR_PORT_DIED` if the server went.

---

## dos/con_open

**NAME**
con_open, con_write, con_puts, con_dec, con_close -- the console

**SYNOPSIS**
```c
#include <planar/dos.h>
void con_open(struct con *c, int64_t sigbit);
void con_write(struct con *c, const char *text, uint32_t len);
void con_puts(struct con *c, const char *text);
void con_dec(struct con *c, uint64_t v);
void con_close(struct con *c);
```

**FUNCTION**
A program's output, as the Amiga's `Output()`. `con_open` connects to the display's console, "con.0". `con_write` sends text there, 64 bytes a message (`DRV_WRITE`); the display draws it and logs each line, so it reaches the serial console too. `con_puts` writes a string, `con_dec` a number in decimal. `con_close` closes it.

Without a console (no framebuffer, or the display gone), text goes to the kernel's log a line at a time, the rest of a line when `con_close` is called.

**EXAMPLE**
From `cmd/echo.c`:
```c
con_open(&c, 16);
con_puts(&c, args);
con_puts(&c, "\n");
con_close(&c);
```

**NOTES**
None returns an error: output always goes somewhere.

**BUGS**
A display that restarts loses the program to the log for good.

**SEE ALSO**
[Devices ch. 3](../devices/03-console.md)

---

## dos/dos_args

**NAME**
dos_args, dos_path -- a command's line and its paths

**SYNOPSIS**
```c
#include <planar/dos.h>
void dos_args(uint64_t arg, char *cwd, unsigned cwd_size, const char **args);
void dos_path(const char *cwd, const char *name, char *out, unsigned size);
```

**FUNCTION**
`dos_args` splits a command's `main` argument, the line the Shell gave `dos_run`, into the current directory (up to its first newline) and the arguments after it. With no line, or none with a newline, `cwd` is "SYS:" and `args` the whole line.

`dos_path` makes `name` a whole path: as it is if it names its volume ("SYS:x"), else under `cwd` ("x" in "SYS:EFI" is "SYS:EFI/x").

**EXAMPLE**
From `cmd/type.c`:
```c
dos_args(arg, cwd, sizeof cwd, &args);
dos_path(cwd, args, path, sizeof path);
f = dos_file(&d, path, 0, &e);
```

**BUGS**
None known.

**SEE ALSO**
[dos_run](#dosdos_run)

---

## dos/dos_file

**NAME**
dos_file, dos_close_file -- open and close a file or directory

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_file(struct dos *d, const char *path, uint32_t flags, struct DosEntry *entry);
int64_t dos_close_file(struct dos *d, uint32_t file);
```

**FUNCTION**
Opens `path` and gives its number, for `dos_read`, `dos_write` and `dos_next`. `DOSF_WRITE` opens it for writing; with `DOSF_CREATE` too a file that isn't there is made, empty. `entry`, if not `nullptr`, gets what it is (`de_Name`, `de_Size`, `de_Dir`, `de_Date`, `de_Time`). A directory is opened to list it.

**RESULT**
The file's number (0 or more), or an error. `dos_close_file` gives 0 or `ERR_BADHANDLE`.

**NOTES**
- A client's files close when its connection does.
- A made file gets an 8.3 name; a name that doesn't fit, or mixes case in a part, gives `ERR_BADARG` ([DOS ch. 1](../dos/01-files-and-programs.md)).

**BUGS**
Two numbers for one file each keep their own size: a file written through one isn't seen to grow through the other.

**SEE ALSO**
[dos_read](#dosdos_read), [dos_next](#dosdos_next)

---

## dos/dos_mkdir

**NAME**
dos_mkdir, dos_remove -- make a directory; remove a file or directory

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_mkdir(struct dos *d, const char *path);
int64_t dos_remove(struct dos *d, const char *path);
```

**FUNCTION**
`dos_mkdir` makes an empty directory, whose parent must exist. `dos_remove` removes a file, or a directory with nothing in it.

**RESULT**
0, or an error: `ERR_EXISTS` (it's there already; a directory not empty), `ERR_BUSY` (the file is open), `ERR_RIGHTS`, `ERR_NOTFOUND`.

**BUGS**
None known.

---

## dos/dos_next

**NAME**
dos_next -- a directory's entries

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_next(struct dos *d, uint32_t dir, uint32_t *cursor, struct DosEntry *entry);
```

**FUNCTION**
Gives the next entry of `dir`, a directory `dos_file` opened. Start `*cursor` at 0. "." and ".." aren't given.

**RESULT**
0, or `ERR_NOTFOUND` after the last.

**EXAMPLE**
From `cmd/list.c`:
```c
while (dos_next(&d, (uint32_t)f, &cursor, &e) == 0) {
    con_puts(&c, e.de_Name);
    ...
}
```

**BUGS**
None known.

---

## dos/dos_open

**NAME**
dos_open, dos_close -- connect to the dos server

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_open(struct dos *d, int64_t sigbit);
void dos_close(struct dos *d);
```

**FUNCTION**
Connects to "dos", with room for one message and a 64 KiB buffer (`DOS_MAX_IO`), and signal bit `sigbit`.

**RESULT**
0, or `ERR_NOTFOUND` if `dos` isn't running (yet: callers at boot try again), `ERR_NOMEM`, or what `ConnectPort` gives.

**BUGS**
None known.

---

## dos/dos_read

**NAME**
dos_read, dos_write -- move bytes

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_read(struct dos *d, uint32_t file, uint64_t off, void *buf, uint32_t len);
int64_t dos_write(struct dos *d, uint32_t file, uint64_t off, const void *buf, uint32_t len);
```

**FUNCTION**
Reads or writes `len` bytes at `off`, 64 KiB a message, copied through the connection's buffer. A read stops at the end of the file. A write past it grows the file.

**RESULT**
The bytes moved, or an error. A read gives fewer than `len` at the end of the file; a write gives `ERR_FULL` if the volume filled.

**NOTES**
A server copies: the data passes through the `dos` connection's heap and then the block connection's.

**BUGS**
None known.

---

## dos/dos_run

**NAME**
dos_run -- load a program from a disk and start it

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_run(struct dos *d, const char *path, const char *args, int64_t sigbit,
                uint32_t flags);
```

**FUNCTION**
Loads the program at `path` into a new Area and starts it with `CreateProcessImage`. The program is an ELF static PIE for this machine, linked as the built-in ones are ([DOS ch. 1](../dos/01-files-and-programs.md#programs)). `args`, if not `nullptr`, is copied (up to `DOS_ARGS`, 1024 bytes) into a page past the image, and the program's `main` gets its address. `sigbit` and `flags` are `CreateProcessImage`'s.

**RESULT**
A process handle, or:
- `ERR_BADARG` -- not a 64-bit little-endian PIE for this machine, more than 16 program headers, or segments that don't fit (64 MiB at most, code ending on a page).
- Any error `dos_file`, `dos_read`, `AllocArea` or `CreateProcessImage` gives.

**EXAMPLE**
From `cmd/shell.c`:
```c
if ((p = dos_run(&dos, path, args, SIG_CHILD, 0)) < 0) {
    say(p == ERR_NOTFOUND ? "Unknown command " : "Can't run ");
    ...
}
while (QueryProcess((Handle)p, &pi) == 0 && pi.pi_State < PROCS_EXITED)
    Wait(1ULL << SIG_CHILD);
```

**NOTES**
The machine's ELF number comes from `arch/<isa>/user/elf.c`.

**BUGS**
None known.

**SEE ALSO**
[CreateProcessImage](exec.md#execcreateprocessimage), [dos_args](#dosdos_args)

---

## dos/dos_stat

**NAME**
dos_stat -- what's at a path

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_stat(struct dos *d, const char *path, struct DosEntry *entry);
```

**FUNCTION**
Fills in `entry` for the file or directory at `path` without opening it.

**RESULT**
0, or `ERR_NOTFOUND`.

**BUGS**
None known.

---

## dos/dos_volume

**NAME**
dos_volume -- the mounted volumes

**SYNOPSIS**
```c
#include <planar/dos.h>
int64_t dos_volume(struct dos *d, uint32_t index, struct DosEntry *entry);
```

**FUNCTION**
Describes volume `index` (from 0): `de_Name` its name, `de_Size` its free bytes, `de_Dir` 1 if it can be written.

**RESULT**
0, or `ERR_NOTFOUND` past the last.

**EXAMPLE**
From `cmd/info.c`:
```c
for (uint32_t i = 0; dos_volume(&d, i, &e) == 0; i++)
    ...
```

**NOTES**
Counting free space reads the whole FAT.

**BUGS**
None known.
