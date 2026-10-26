# The Shell

*Checked against V6; the desktop and `SERIAL`, V8 step 4; in a window, V8 step 5.*

## What it's for

Typing commands. The Shell (`cmd/shell.c`) is itself a program on the disk, `C:shell`. It reads lines from the serial console, and from a USB keyboard when there's no desktop, runs each as a command loaded from `C:`, and its output, and the commands', goes to the console: the screen and the serial line both. On the desktop, a Shell runs in a window of its own (below).

Its template is `FROM,SERIAL/S,CON/K`. `FROM` runs a script's lines and ends (below). `SERIAL` reads the serial console alone, leaving the keyboard to intuition: hwdb starts the console Shell so when it starts the desktop. `CON` names a console window to run in (`Shell CON con.1`); a Shell a window starts is told it by its line instead.

## Booting to it

`BOOTARGS=shell` boots to the Shell: the kernel skips its task tests and starts hwdb in its normal mode. hwdb starts the drivers, then `dos`, then `C:shell SERIAL`, gives the Shell the console's input (`GrantConsole`), and starts either again if it ends. Once display has a screen, hwdb starts intuition too, which takes the mouse and the keyboard and shows the desktop over the console (V8 step 4; [Libraries ch. 13](../libraries/13-intuition.md)). `BOOTARGS="shell nointuition"` is V7's boot: no desktop, and `C:shell` reading the keyboard as well. On QEMU:

```
make PLATFORM=qemu-virt-a64 EFI=1 run BOOTARGS=shell
```

(Through edk2 with `ramfb` there's a framebuffer; with `-kernel` the output goes to the serial line alone. RV64 QEMU has a `ramfb` too, set up by the kernel, though `-nographic` keeps its window shut: QMP's `screendump` sees it.) On the CIX, `make PLATFORM=uefi-a64 run BOOTARGS=shell`. A board run adds `reboot`, so `EndShell` ends the system and the board starts over into its other system.

## In a window

The system menu's *Shell* (the Planar mark, at the left of the screen bar) opens a Shell in a window: a console window ([Devices ch. 3](../devices/03-console.md#console-windows)), `Shell 2` for the first, with the Shell in it on "con.1". Up to eight are open at once, each its own console and its own Shell. Its keyboard is the desktop's, when its window is the active one:

```
Planar Shell. Type Help for help.
2.SYS:> Shell FROM S:window
2.SYS:> ; A Shell in a window (V8 step 5): conformance/window.c types Shell FROM S:window
2.SYS:> Echo "Planar's Shell, in a window, in System Mono."
Planar's Shell, in a window, in System Mono.
...
```

- The prompt is green, and its number is the console's and 1: `2.` on "con.1".
- The window edits the line, as the Amiga's `CON:` does: Backspace, Ctrl-U or Ctrl-X to delete it all, Return to run it. What's typed while a command runs waits for the Shell.
- Every command it runs writes to the window too: it's given the console with its line ([SplitArgs](../autodocs/dos.md#dossplitargs)), and so is a Shell it runs, as `Shell FROM` above.
- Page Up and Page Down scroll back through the last 1,000 lines; Shift with the arrows, a line.
- `EndShell`, or Ctrl-\ (the end of the input), ends the Shell and closes the window. The close gadget ends both at once, and the command running.

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

**Scripts.** `Shell FROM FILE` (its template is `FROM`) runs a file's lines, each shown after the prompt as if typed, and ends with the number that failed. Scripts are in `S:` (`SYS:EFI/planar/S`, from `cmd/scripts`). hwdb's test mode runs `C:shell FROM SYS:EFI/planar/S/test` (`cmd/scripts/test`) on every test boot, which uses each command's template: keywords, `=`, quotes and switches; its window test types `Shell FROM S:window` at a Shell in a window.

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

- Its line is the Shell's current directory, a newline, then its arguments: split it with `SplitArgs`, read the arguments with `ReadArgs`, and make each path whole with `AddPart` on a copy of the directory. From a Shell in a window, the line also names the window's console, which `SplitArgs` makes the process's: call it before `OpenCon`.
- On a `ReadArgs` error, say `rda_Why` and the template, as above: `rda_Why` is empty for `?`.
- Write with `PutStr` and `Printf`. Exit 0 for success.

## Rules

- The console Shell's input is polled every 10 ms: the serial console's (`ReadConsole`), and, unless `SERIAL`, the keys typed on a USB keyboard, from `input`'s ring ([Devices ch. 7](../devices/07-input.md)), which the Shell takes when `input` is there and again after it restarts. Another client holding the ring keeps it (intuition, with the desktop): the Shell then has the serial console alone.
- A Shell in a window waits for its window's lines (`ReadCon`) and polls nothing. If the window goes, the Shell goes with it: it's the console window's process, and ends when that does.
- A command owns nothing the Shell needs: it ends, and the Shell carries on.

## Differences from AmigaDOS

- No `Run`, pipes or redirection yet, and no `Execute`; `Shell FROM` is the one way to run a script, and it ends when the script does (AmigaDOS's `NewShell FROM` stays). There's no `NewShell` command: the system menu opens a Shell in a window, and a Shell can't open one itself yet.
- A Shell in a window's prompt is coloured, by a CSI sequence the window reads; the console Shell's isn't.
- The commands' templates are AmigaDOS's where the command does the same job, and shorter: `Delete` has no `ALL` or `FORCE`, `List` no patterns or dates, `Type` no `HEX` or `TO`.
- `?` shows the template and the command ends; AmigaDOS's then read a new line.
- Command names in `C:` are lower case on the disk; typing any case finds them.

## Functions used

[RunCommand](../autodocs/dos.md#dosruncommand), [SplitArgs](../autodocs/dos.md#dossplitargs), [ReadArgs](../autodocs/dos.md#dosreadargs), [AddPart](../autodocs/dos.md#dosaddpart), [OpenCon](../autodocs/dos.md#dosopencon), [SetConName](../autodocs/dos.md#dossetconname), [Printf](../autodocs/dos.md#dosprintf), [GrantConsole](../autodocs/lease.md#leasegrantconsole), [ReadConsole](../autodocs/lease.md#leasereadconsole)
