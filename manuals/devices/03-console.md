# The console class

*Checked against V5; console windows, V8 step 5.*

## What it's for

Text to the screen. The display driver (`drv/display`) draws a text console on the framebuffer the firmware left set up, which is its output's plane 0, and publishes it as "con.0" (the planes above it are [chapter 10](10-display.md)'s). Every line it draws also goes to the kernel's log, and so to the serial console: the console is both (v4-plan decision 1).

Input is the serial console's, given to one process by the device root's holder: `GrantConsole` and `ReadConsole` ([lease](../autodocs/lease.md#leasegrantconsole)). hwdb gives it to the Shell, which reads a USB keyboard's keys too, from `input` ([chapter 7](07-input.md)).

## Concepts

- A client connects to "con.0" and sends `struct DrvRequest` messages with `dr_Op` `DRV_WRITE` and up to 64 bytes of text in `dr_Data` (`dr_Len` of them).
- The console understands newline, backspace and tab (as a space). It wraps at the screen's edge and scrolls a line at a time.
- Text is the 8×8 font, twice the size on screens 1600 pixels wide or more: 1920×1080 on the CIX is 120 columns by 67 lines.
- A write of 0 bytes does nothing, and is answered: the kill test's client pings with it.

Where there's no framebuffer (QEMU booted with `-kernel`), there's no "con.0".

## Console windows

A console window (`servers/conwin`, V8 step 5) is the same class in a window on the desktop: the system menu's *Shell* starts one, and it starts a Shell in it ([dos ch. 2](../dos/02-shell.md#in-a-window)). Each is a process of its own, which hwdb starts and holds, publishing the first of "con.1" to "con.8" that's free. It serves what "con.0" does, and reading too:

- `DRV_WRITE`: text, drawn in System Mono 13 px on `pane.terminal.bg` (§13.2), not logged. UTF-8; newline (and column 0, as the Amiga's `CON:`), carriage return, backspace (a column back), tab (to the next of every 8th column), form feed (the window cleared). CSI sequences, `ESC [` or the Amiga's `0x9b`: `m` (0 plain, 1 bold, 22 not, 30 to 37 and 90 to 97 a colour, 39 the text's; 32 is the prompt's green), `K` (the line cleared from the cursor), `2J` (the window cleared). Other sequences are read and dropped. A line runs on into the next at the window's edge.
- `DRV_READ`: what's typed, up to `dr_Len` bytes, answered once a line is ended with Return. The window edits the line as it's typed, as `CON:` does: echoed, Backspace deletes, Ctrl-U or Ctrl-X the whole line. Ctrl-\ is the end of the input: a read waiting then is answered with no bytes. One read waits on a connection at a time; a second is `ERR_BUSY`.
- The scrollback is the last 1,000 lines. Page Up and Page Down go back and forth through it a window at a time, Shift with Up and Down a line, Shift with Home and End to either end; typing goes back to the bottom.
- Resizing the window by its corner shows as many cells as fit, and reflows nothing: lines written wider are cut at the edge, as a terminal cuts them.
- The window's close gadget ends it and its Shell, and the command the Shell's running. The Shell ending closes the window. If intuition dies, the window opens again on the next intuition, drawn from the scrollback; the Shell sees nothing of it.

## Using it

`dos.library` has the client: `OpenCon`, `WriteCon`, `PutStr`, `Printf`, `CloseCon`, and `ReadCon` for a console window's lines. `Printf` gathers its bytes 64 at a time, one message each. Without a console, they write to the kernel's log a line at a time, so a program needn't care. `OpenCon` opens the process's console, "con.0" unless `SetConName` said otherwise; a command started by a Shell in a window is given the window's (`SplitArgs`), so its output goes there.

From `cmd/info.c`:
```c
struct Con c;

OpenCon(&c, 16);
PutStr(&c, "Volumes:\n");
...
Printf(&c, "  %s: %lu MiB free, %s\n", e.de_Name, e.de_Size >> 20, ...);
CloseCon(&c);
```

## Rules

- A line on "con.0" also goes to the log: don't write what mustn't be on the serial line. A console window's doesn't.
- Output from many programs at once interleaves by message: send whole lines.

## Functions used

[OpenCon](../autodocs/dos.md#dosopencon), [Printf](../autodocs/dos.md#dosprintf), [SetConName](../autodocs/dos.md#dossetconname), [GrantConsole](../autodocs/lease.md#leasegrantconsole), [ReadConsole](../autodocs/lease.md#leasereadconsole)
