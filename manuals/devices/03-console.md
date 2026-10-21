# The console class

*Checked against V4.*

## What it's for

Text to the screen. The display driver (`drv/display`) draws a text console on the framebuffer the firmware left set up, which is its output's plane 0, and publishes it as "con.0" (the planes above it are [chapter 10](10-display.md)'s). Every line it draws also goes to the kernel's log, and so to the serial console: the console is both (v4-plan decision 1).

Input is the serial console's, given to one process by the device root's holder: `GrantConsole` and `ReadConsole` ([lease](../autodocs/lease.md#leasegrantconsole)). hwdb gives it to the Shell, which reads a USB keyboard's keys too, from `input` ([chapter 7](07-input.md)).

## Concepts

- A client connects to "con.0" and sends `struct DrvRequest` messages with `dr_Op` `DRV_WRITE` and up to 64 bytes of text in `dr_Data` (`dr_Len` of them).
- The console understands newline, backspace and tab (as a space). It wraps at the screen's edge and scrolls a line at a time.
- Text is the 8×8 font, twice the size on screens 1600 pixels wide or more: 1920×1080 on the CIX is 120 columns by 67 lines.
- A write of 0 bytes does nothing, and is answered: the kill test's client pings with it.

Where there's no framebuffer (QEMU booted with `-kernel`), there's no "con.0".

## Using it

`dos.library` has the client: `con_open`, `con_write`, `con_puts`, `con_dec`, `con_close`. Without a console, they write to the kernel's log a line at a time, so a program needn't care.

From `cmd/info.c`:
```c
struct con c;

con_open(&c, 16);
con_puts(&c, "Volumes:\n");
...
con_close(&c);
```

## Rules

- A console line also goes to the log: don't write what mustn't be on the serial line.
- Output from many programs at once interleaves by message: send whole lines.

## Functions used

[con_open](../autodocs/dos.md#doscon_open), [GrantConsole](../autodocs/lease.md#leasegrantconsole), [ReadConsole](../autodocs/lease.md#leasereadconsole)
