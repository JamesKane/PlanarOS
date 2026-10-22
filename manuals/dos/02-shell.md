# The Shell

*Checked against V6.*

## What it's for

Typing commands. The Shell (`cmd/shell.c`) is itself a program on the disk, `C:shell`. It reads lines from the serial console and from a USB keyboard, runs each as a command loaded from `C:`, and its output, and the commands', goes to the console: the screen and the serial line both.

## Booting to it

`BOOTARGS=shell` boots to the Shell: the kernel skips its task tests and starts hwdb in its normal mode. hwdb starts the drivers, then `dos`, then `C:shell`, gives the Shell the console's input (`GrantConsole`), and starts either again if it ends. On QEMU:

```
make PLATFORM=qemu-virt-a64 EFI=1 run BOOTARGS=shell
```

(Through edk2 with `ramfb` there's a framebuffer; with `-kernel` the output goes to the serial line alone. RV64 QEMU has a `ramfb` too, set up by the kernel, though `-nographic` keeps its window shut: QMP's `screendump` sees it.) On the CIX, `make PLATFORM=uefi-a64 run BOOTARGS=shell`. A board run adds `reboot`, so `EndShell` ends the system and the board starts over into its other system.

## Using it

The prompt is the CLI's number and the current directory:

```
1.SYS:> cd SYS:EFI/planar
1.SYS:EFI/planar> list C
Directory SYS:EFI/planar/C
  delete  104352
  ...
1.SYS:EFI/planar> type S/greeting number
   1 Hello from a file on the disk, typed by C:type.
1.SYS:EFI/planar> delete
Delete: FILE is required. Template: FILE/M/A,QUIET/S
delete failed returncode 1
1.SYS:EFI/planar> delete ?
Delete: FILE/M/A,QUIET/S
delete failed returncode 1
```

**Built-ins:**

| Command | |
|---|---|
| `CD [DIR]` | Change the current directory; with none, show it. `CD /` goes up a level, `CD :` to the volume's root (`AddPart`'s rules). |
| `Help` | What's built in. |
| `EndShell` (or `EndCLI`) | End the Shell. |

**Commands in `C:`**, each with its `ReadArgs` template:

| Command | Template | |
|---|---|---|
| `Info` | `VOLUME` | The volumes, or the one named, their free space, and whether they can be written |
| `List` | `DIR/M,FILES/S,DIRS/S,QUICK/S` | Directories' entries and sizes, and the totals (the current directory if none is named); `FILES` or `DIRS` only those, `QUICK` names only |
| `Type` | `FROM/A/M,NUMBER/S` | Files' text; `NUMBER` numbers the lines |
| `Echo` | `TEXT/M,NOLINE/S` | Its words, a space between each; `NOLINE` no newline after |
| `Touch` | `FILE/M/A` | Makes each file, empty, if it isn't there |
| `Delete` | `FILE/M/A,QUIET/S` | Deletes files, or empty directories, saying which unless `QUIET` |

As on the Amiga, a command's arguments go in its template's order or by keyword, in any case: `Type S/greeting NUMBER`, `type number from=S/greeting`, `List DIR=SYS:EFI QUICK`. A word in quotes is never a keyword, and may hold spaces: `Echo "Two  spaces"`. `?` alone shows the template. A command given arguments its template doesn't take says why, and shows it.

Anything else is looked for as `C:NAME`, or as the path it names if it has a `:` or `/`. The rest of the line is its arguments. The Shell waits for it, and says "failed returncode N" if it ended badly, "Unknown command" if there's no such program.

Backspace edits the line. Lines starting with `;` are comments.

**Scripts.** `Shell FROM FILE` (its template is `FROM`) runs a file's lines, each shown after the prompt as if typed, and ends with the number that failed. hwdb's test mode runs `C:shell FROM SYS:EFI/planar/S/test` (`cmd/scripts/test`) on every test boot, which uses each command's template: keywords, `=`, quotes and switches.

A Shell started from a Shell begins in its current directory.

## Writing a command

A command is one file in `cmd/`, built and installed by the Makefile. From `cmd/echo.c`:

```c
static const char TEMPLATE[] = "TEXT/M,NOLINE/S";

int main(uint64_t arg)
{
    struct Con c;
    struct RDArgs ra = { 0 };
    int64_t a[2] = { 0 };
    char cwd[DOS_PATH];

    SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
    OpenCon(&c, 16);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&c, "Echo: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&c);
        return 1;
    }
    for (const char *const *w = (const char *const *)a[0]; w && *w; w++)
        Printf(&c, w[1] ? "%s " : "%s", *w);
    if (!a[1])
        PutStr(&c, "\n");
    FreeArgs(&ra);
    CloseCon(&c);
    return 0;
}
```

- Its line is the Shell's current directory, a newline, then its arguments: split it with `SplitArgs`, read the arguments with `ReadArgs`, and make each path whole with `AddPart` on a copy of the directory.
- On a `ReadArgs` error, say `rda_Why` and the template, as above: `rda_Why` is empty for `?`.
- Write with `PutStr` and `Printf`. Exit 0 for success.

## Rules

- Input is polled every 10 ms: the serial console's (`ReadConsole`), and the keys typed on a USB keyboard, from `input`'s ring ([Devices ch. 7](../devices/07-input.md)), which the Shell takes when `input` is there and again after it restarts. Another client holding the ring keeps it: the Shell then has the serial console alone.
- A command owns nothing the Shell needs: it ends, and the Shell carries on.

## Differences from AmigaDOS

- No `Run`, pipes or redirection yet, and no `Execute`; `Shell FROM` is the one way to run a script, and it ends when the script does (AmigaDOS's `NewShell FROM` stays).
- The commands' templates are AmigaDOS's where the command does the same job, and shorter: `Delete` has no `ALL` or `FORCE`, `List` no patterns or dates, `Type` no `HEX` or `TO`.
- `?` shows the template and the command ends; AmigaDOS's then read a new line.
- Command names in `C:` are lower case on the disk; typing any case finds them.

## Functions used

[RunCommand](../autodocs/dos.md#dosruncommand), [SplitArgs](../autodocs/dos.md#dossplitargs), [ReadArgs](../autodocs/dos.md#dosreadargs), [AddPart](../autodocs/dos.md#dosaddpart), [OpenCon](../autodocs/dos.md#dosopencon), [Printf](../autodocs/dos.md#dosprintf), [GrantConsole](../autodocs/lease.md#leasegrantconsole), [ReadConsole](../autodocs/lease.md#leasereadconsole)
