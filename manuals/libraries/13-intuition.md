# 13. intuition: windows on the screen

*Written, V7 step 3. Input, focus, the chrome and dragging come with step 4.*

intuition puts windows on the screen. A program draws a window's pixels with gfx.library (chapter 12) into bitmaps of its own, and presents each frame. intuition composes the screen from every window's latest frame, on the CPU, and shows it. A window's pixels never cross IPC: intuition reads them where the program drew them, shared read-only ([docs §13.1](../../docs/13-workbench.md)). It never writes a program's memory, and never waits for one.

There are two parts:
- **intuition, the server** (`servers/intuition`), display's client. It composes into two full-screen buffers of display's, shown in turn on plane 1, over the console on plane 0. The pointer is plane 2. When intuition goes, the console shows again.
- **intuition.library**, the program's side, reached through `IIntuition`. Its functions are in `planar/intuition.h` and `lib/intuition`. It keeps no state: the connection is the caller's `Intuition`, and each window the caller's `Window`.

intuition runs when it's wanted, not at every boot (v7-plan decision 4): the console Shell keeps the keyboard until V8 puts it in a window. hwdb starts it for its window test.

## Concepts

- **The screen** is the Workbench's, the only one, the size of display's output: 1280×800 on RV64 QEMU, 800×600 on AArch64's, 1920×1080 on the boards. Its backdrop is `bg.backdrop.base` (§13.2), flat for now.
- **A window** is a rectangle on the screen, anywhere, on it or off its edges. Its pixels are the top left of one of its **buffers**: one or two bitmaps of the program's, from `AllocBitMap`. Windows are opaque in V7. In V7 step 3 a window is all inside, with no title bar or frame: those are step 4's.
- **A frame** is what a window shows. The program draws it in the window's back buffer and presents it, saying what changed since the last frame. Frames are numbered from 1, in `w_Frame`.
- **Depth.** A window comes to the front of the screen at its first frame, and is in front of all those that came before it.
- **The sync words.** Each window has a cache line of a page intuition shares read-only with the program, `w_Sync`, as display's sync page is for planes:
  - `is_Safe`, the last frame intuition has composed: it reads nothing of the window's other buffer any more.
  - `is_Disp`, the last frame on the screen, and `is_DispTime`, the vblank it latched at.

## Opening a window

From `conformance/window.c`, shortened:
```c
#include <planar/intuition.h>

struct Library *lib = IExec->OpenLibrary("intuition.library", 7);
struct IntuitionIFace *IIntuition =
    lib ? (struct IntuitionIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
struct BitMap *front = IGfx->AllocBitMap(200, 120, PIXFMT_ARGB8888, 0, nullptr);
struct BitMap *back = IGfx->AllocBitMap(200, 120, PIXFMT_ARGB8888, 0, nullptr);
struct Intuition in;
struct Window w;

IIntuition->OpenIntuition(&in, CurrentTime() + 5000 * MS);
OpenWindowTags(IIntuition, &in, &w, { WA_Left, 64 }, { WA_Top, 48 }, { WA_Title, (uint64_t)"wa" },
               { WA_BitMap, (uint64_t)front }, { WA_BackBitMap, (uint64_t)back });
picture(&w.w_RPort, 200, 120, 0x5b88c2, "window a");      /* gfx.library's calls */
IIntuition->PresentWindow(&w, nullptr, 0);                 /* all of it: the first frame */
```

`OpenWindowTags` is a macro over `OpenWindowTagList`, making the tag list from its arguments. `OpenWindowTagList` shares each bitmap's Area with intuition, read-only, and sends their addresses and strides. intuition checks them against the Areas it has been given, and describes each one itself. It never reads the `BitMap`s in the program's Areas.

## Presenting

A frame is drawn through `w.w_RPort`, which `PresentWindow` keeps on the back buffer:
```c
static const struct GfxRect bar = { 16, 40, 111, 55 };

for (;;) {
    if (IIntuition->WaitSafe(&w, CurrentTime() + 1000 * MS) != 0)
        break;                          /* intuition went, or stopped composing */
    tick(&w.w_RPort, w.w_Frame + 1);    /* draws inside 'bar' alone */
    IIntuition->PresentWindow(&w, &bar, 1);
}
```

- **`PresentWindow` doesn't wait.** It sends the frame's buffer, its damage and its number, and returns. intuition takes the latest frame of each window as it composes, so one presented before the last was composed is never shown: nothing waits for it.
- **`WaitSafe` waits for `is_Safe`**, then the back buffer is free. If it's free already, `WaitSafe` makes no kernel call. A program that presents faster than the screen sleeps there, once a frame. One that's slower never does.
- **Damage is what changed since the last frame presented**, in the window's coordinates. With two buffers, the back one is a frame old: bring it up to date with the last frame's changes before drawing the next. `window.c` blits the front buffer across once, then redraws the same rectangle each frame. No damage (a count of 0) is the whole window.
- **Frames go to the screen at the vblank.** intuition composes a frame when the last one has latched: one a vblank at most, for all the windows together. `is_Disp` says when a frame's pixels are there.

## How intuition composes

- **Damage.** intuition keeps the rectangles of the screen that changed: a window's damage moved to where it is, or a window's whole rectangle when it opens, closes or moves. A frame is composed into the buffer not on the screen, which is a frame old, so each frame repaints what changed for it and for the last.
- **Front to back.** A rectangle to repaint is taken by the windows in depth order. The front one it meets gives the part they share, blitted from its buffer. The pieces beside that go on to the windows behind, and what none covers is the backdrop. Each pixel is written once, and the screen is never read, so it can be write-combined memory (v7-plan decision 3).
- **gfx.library's blits.** intuition composes with `IGfx->BltBitMap` and `RectFill`, by the vector unit where the CPU has one.
- **The pointer** is a 16×16 sprite on plane 2 (§13.2's `cursor.fill`), at the screen's centre until step 4 moves it. Moving it needs no window composed.

`make intuition-test` runs the compositor on the host with the sanitizers. Over made-up windows opened, moved, presented, raised and closed at random, every frame is checked pixel for pixel against a compositor that redraws the whole screen. A watch on gfx.library's table checks that no pixel is written twice in a frame.

## Costs

Measured by `conformance/window.c` under QEMU (TCG), on intuition's buffers, which are cached on QEMU. V7 step 6 measures the boards.

| | RV64 | AArch64 |
|---|---|---|
| Composing, a megapixel | 2.6-3.5 ms | 1.9-2.0 ms |
| Present to latch, a frame at a time | 14-21 ms | 10-18 ms |
| The program's kernel calls a frame, presenting as fast as `is_Safe` allows | 3 | 3 |
| Frames shown of 120 so presented | 120, in 120 to 124 vblanks | 120, in 120 vblanks |

The 3 calls are `WaitSafe`'s `WaitAddr`, the signal `PresentWindow`'s message gives a sleeping intuition, and `window.c`'s own `CurrentTime` for `WaitSafe`'s deadline. A frame presented a frame at a time takes one to two vblanks to latch: it waits for intuition's next frame, then for display's latch.

## Rules

- **Check what `OpenWindowTagList` returns.** It's `ERR_BADARG` for a window bigger than a bitmap, or one that isn't `AllocBitMap`'s, and `ERR_RIGHTS` for memory intuition can't read.
- **Don't draw in a buffer until it's safe.** The back buffer is intuition's to read until `WaitSafe` returns. The program's own memory can't hurt intuition, but a frame drawn early may be shown half drawn.
- **Don't unshare a window's bitmap while it's open.** intuition reads it until `CloseWindow`, and a read of memory taken away ends it (`UnshareArea`).
- **Free the bitmaps after `CloseWindow`**, not before: intuition still maps them until then, and the memory lasts while it does.
- **Presenting can fail late.** A refusal of intuition's comes back with the next `PresentWindow` or `WaitSafe`.

## Differences from the RKM

| Intuition | intuition, V7 |
|---|---|
| `OpenWindowTags(NULL, WA_..., TAG_END)` returns a `Window *` | `OpenWindowTags(IIntuition, &in, &w, { WA_..., ... })` fills in the caller's `Window` and returns an error |
| A window draws in place, through a `RastPort` the layers clip | A window draws in bitmaps of its own, and presents frames; intuition composes them |
| Simple, smart and super bitmap refresh, and `IDCMP_REFRESHWINDOW` | No refresh: a window's last frame is always there to compose from |
| `IntuitionBase`, screens, `OpenWorkbench` | One screen, the Workbench's, reached through the caller's `Intuition` |
| Gadgets, menus, IDCMP | V7 step 4 (IDCMP, the chrome), then V8 (ui.library) |
| `WaitTOF` | `WaitSafe`, on a window's own `is_Safe` |

## Functions used

[CloseIntuition](../autodocs/intuition.md#intuitioncloseintuition), [CloseWindow](../autodocs/intuition.md#intuitionclosewindow), [OpenIntuition](../autodocs/intuition.md#intuitionopenintuition), [OpenWindowTagList](../autodocs/intuition.md#intuitionopenwindowtaglist), [PresentWindow](../autodocs/intuition.md#intuitionpresentwindow), [WaitSafe](../autodocs/intuition.md#intuitionwaitsafe); [AllocBitMap](../autodocs/gfx.md#gfxallocbitmap), [BltBitMap](../autodocs/gfx.md#gfxbltbitmap), [ShareArea](../autodocs/exec.md#execsharearea), [WaitAddr](../autodocs/exec.md#execwaitaddr).
