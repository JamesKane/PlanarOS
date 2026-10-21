# The Shell

*Checked against V4 step 7.*

## What it's for

Typing commands. The Shell (`cmd/shell.c`) is itself a program on the disk, `C:shell`. It reads lines from the serial console, runs each as a command loaded from `C:`, and its output, and the commands', goes to the console: the screen and the serial line both.

## Booting to it

`BOOTARGS=shell` boots to the Shell: the kernel skips its task tests and starts hwdb in its normal mode. hwdb starts the drivers, then `dos`, then `C:shell`, gives the Shell the console's input (`GrantConsole`), and starts either again if it ends. On QEMU:

```
make PLATFORM=qemu-virt-a64 EFI=1 run BOOTARGS=shell
```

(Through edk2 with `ramfb` there's a framebuffer; with `-kernel` the output goes to the serial line alone.) On the CIX, `make PLATFORM=uefi-a64 run BOOTARGS=shell`. A board run adds `reboot`, so `EndShell` ends the system and the board starts over into its other system.

## Using it

The prompt is the CLI's number and the current directory:

```
1.SYS:> cd SYS:EFI/planar
1.SYS:EFI/planar> list C
Directory SYS:EFI/planar/C
  echo  84416
  ...
1.SYS:EFI/planar> type S/greeting
Hello from a file on the disk, typed by C:type.
```

**Built-ins:**

| Command | |
|---|---|
| `CD [DIR]` | Change the current directory; with none, show it. `CD /` goes up a level. |
| `Help` | What's built in. |
| `EndShell` (or `EndCLI`) | End the Shell. |

**Commands in C:** `List [DIR]`, `Type FILE`, `Echo ...`, `Info`, `Touch FILE` (make an empty file), `Delete NAME` (a file, or an empty directory).

**Commands in `C:`:**

| Command | |
|---|---|
| `Info` | The volumes, their free space, and whether they can be written |
| `List [DIR]` | A directory's entries and sizes, and the totals |
| `Type FILE` | A file's text |
| `Echo TEXT` | Its text |

Anything else is looked for as `C:NAME`, or as the path it names if it has a `:` or `/`. The rest of the line is its arguments. The Shell waits for it, and says "failed returncode N" if it ended badly, "Unknown command" if there's no such program.

Backspace edits the line. Lines starting with `;` are comments.

**Scripts.** `Shell -s FILE` runs a file's lines, each shown after the prompt as if typed, and ends with the number that failed. hwdb's test mode runs `C:shell -s SYS:EFI/planar/S/test` (`cmd/scripts/test`) on every test boot.

## Writing a command

A command is one file in `cmd/`, built and installed by the Makefile. From `cmd/echo.c`:

```c
int main(uint64_t arg)
{
    struct con c;
    char cwd[DOS_PATH];
    const char *args;

    dos_args(arg, cwd, sizeof cwd, &args);
    con_open(&c, 16);
    con_puts(&c, args);
    con_puts(&c, "\n");
    con_close(&c);
    return 0;
}
```

- Its line is the Shell's current directory, a newline, then its arguments: split it with `dos_args`, and make paths with `dos_path`.
- Write with `con_*`. Exit 0 for success.

## Rules

- Input is polled every 10 ms. There's no keyboard yet: the serial console is the input (v4-plan decision 1; USB in V5).
- A command owns nothing the Shell needs: it ends, and the Shell carries on.

## Differences from AmigaDOS

- No `Run`, pipes or redirection yet, and no `Execute`; `-s` is the one way to run a script.
- Command names in `C:` are lower case on the disk; typing any case finds them.

## Functions used

[dos_run](../autodocs/dos.md#dosdos_run), [dos_args](../autodocs/dos.md#dosdos_args), [con_open](../autodocs/dos.md#doscon_open), [GrantConsole](../autodocs/lease.md#leasegrantconsole), [ReadConsole](../autodocs/lease.md#leasereadconsole)
