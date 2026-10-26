# 13. intuition: windows on the screen

*Written, V7 step 3; input, focus, the chrome and dragging, V7 step 4; servers dying, V7 step 5.*

intuition puts windows on the screen. A program draws a window's pixels with gfx.library (chapter 12) into bitmaps of its own, and presents each frame. intuition composes the screen from every window's latest frame, on the CPU, and shows it. A window's pixels never cross IPC: intuition reads them where the program drew them, shared read-only ([docs §13.1](../../docs/13-workbench.md)). Around them it draws the window's chrome, its own pixels. It holds the mouse and the keyboard, moves the pointer, and drags and depth-arranges windows itself, telling each program what it asked to hear. It never writes a program's memory, and never waits for one.

There are two parts:
- **intuition, the server** (`servers/intuition`), display's client. It composes into two full-screen buffers of display's, shown in turn on plane 1, over the console on plane 0. The pointer is plane 2. When intuition goes, the console shows again.
- **intuition.library**, the program's side, reached through `IIntuition`. Its functions are in `planar/intuition.h` and `lib/intuition`. It keeps no state: the connection is the caller's `Intuition`, and each window the caller's `Window`.

intuition runs when it's wanted, not at every boot (v7-plan decision 4): the console Shell keeps the keyboard until V8 puts it in a window. hwdb starts it for its window test, and then keeps it as it keeps input: started again 10 ms after it ends, later each time if it keeps ending at once, until it's no longer wanted. If another program holds input's ring when intuition starts, intuition goes on without the mouse and keys, and says so.

## Concepts

- **The screen** is the Workbench's, the only one, the size of display's output: 1280×800 on RV64 QEMU, 800×600 on AArch64's, 1920×1080 on the boards. Its backdrop is `bg.backdrop.base` (§13.2), flat for now.
- **A window** is a rectangle on the screen, anywhere, on it or off its edges. Its **inside** is the top left of one of its **buffers**: one or two bitmaps of the program's, from `AllocBitMap`. Around the inside is its **chrome**, intuition's: a title bar, gadgets and a frame, unless it's opened `WFLG_BORDERLESS`. Windows are opaque in V7.
- **The pointer** is intuition's, moved by the mouse, and at the screen's centre when intuition starts. The window a button is pressed over becomes the **active** one, and the keys go to it.
- **A frame** is what a window shows. The program draws it in the window's back buffer and presents it, saying what changed since the last frame. Frames are numbered from 1, in `w_Frame`.
- **Depth.** A window comes to the front of the screen at its first frame, and is in front of all those that came before it.
- **The sync words.** Each window has a cache line of a page intuition shares read-only with the program, `w_Sync`, as display's sync page is for planes:
  - `is_Safe`, the last frame intuition has composed: it reads nothing of the window's other buffer any more.
  - `is_Disp`, the last frame on the screen, and `is_DispTime`, the vblank it latched at;
  - `is_LeftEdge` and `is_TopEdge`, where the window is now, which is where it was dragged.
- **Events** are what a window's program hears of it: the IDCMP classes it asked for, on a ring in the same page.

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
               { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE },
               { WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS },
               { WA_BitMap, (uint64_t)front }, { WA_BackBitMap, (uint64_t)back });
picture(&w.w_RPort, 200, 120, 0x5b88c2, "window a");      /* gfx.library's calls */
IIntuition->PresentWindow(&w, nullptr, 0);                 /* all of it: the first frame */
```

`OpenWindowTags` is a macro over `OpenWindowTagList`, making the tag list from its arguments. `OpenWindowTagList` shares each bitmap's Area with intuition, read-only, and sends their addresses and strides. intuition checks them against the Areas it has been given, and describes each one itself. It never reads the `BitMap`s in the program's Areas.

`WA_Width` and `WA_Height` are the inside's, and `WA_Left` and `WA_Top` the chrome's top left. With chrome, the window is `INTUI_BORDER` (4) pixels wider on each side, `INTUI_TITLEBAR` (26) taller above and 4 below; `w.w_BorderLeft` and `w.w_BorderTop` say where the inside is.

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

## The chrome

intuition draws it with gfx.library, in §13.2's colours, into two small bitmaps of its own for each window: one for the title bar and the frame's bottom, one for its sides. They're composed as parts of the window, beside its inside, so the program's buffers are never written.

```
┌──────────────────────────────────┬──┬──┐  title bar, 26 px:
│ title                            │◰ │✕ │  the depth gadget, then the close one
├──────────────────────────────────┴──┴──┤
│                                        │
│              the inside                │  the frame, 4 px, bevelled
│                                        │
└────────────────────────────────────────┘
```

- **The title bar** is `win.frame.active`'s gradient on the active window and `win.frame.inactive`'s on the others, with the title in the 8×8 font.
- **Dragging** (`WFLG_DRAGBAR`): the left button held on the title bar moves the window with the pointer. Each move repaints where the window was and where it is.
- **The depth gadget** (`WFLG_DEPTHGADGET`): a click sends the window to the back, or to the front if it's at the back already.
- **The close gadget** (`WFLG_CLOSEGADGET`): a click sends `IDCMP_CLOSEWINDOW`. The program closes the window. If it hasn't within a second, intuition takes the window off the screen itself, and says so in the log (v7-plan decision 6).

A gadget acts when the button is let go over it, and is drawn pressed while it's held there. Iconify, zoom, the border scrollers and resizing are V8's, with `ui.library`.

## Focus and events

A button pressed over a window makes it the active one, and brings it to the front, unless the press is on its depth gadget. Keys go to the active window alone. A press over the backdrop leaves no window active.

A window hears what it asked for with `WA_IDCMP`, the RKM's classes:

| Class | When | `iev_Code` |
|---|---|---|
| `IDCMP_MOUSEBUTTONS` | a button pressed over its inside, and let go | `SELECTDOWN`, `SELECTUP`, `MENUDOWN` ... |
| `IDCMP_MOUSEMOVE` | the pointer moved, while it's active | |
| `IDCMP_RAWKEY` | a key, while it's active | the HID usage, `\| RAWKEY_UP` let go |
| `IDCMP_VANILLAKEY` | a key that types a character, going down | the character |
| `IDCMP_ACTIVEWINDOW`, `IDCMP_INACTIVEWINDOW` | it became the active one, or stopped | |
| `IDCMP_CLOSEWINDOW` | its close gadget was clicked | |

Mouse positions are the inside's coordinates. The events of all a connection's windows come on one ring, each with its window's `w_Id`:
```c
struct IntuiEvent ev;

while (IIntuition->GetIntuiEvent(&in, &ev)) {
    if (ev.iev_Window != w.w_Id)
        continue;
    if (ev.iev_Class == IDCMP_CLOSEWINDOW)
        done = true;
    else if (ev.iev_Class == IDCMP_VANILLAKEY)
        typed(ev.iev_Code);
}
IIntuition->WaitIntuiEvent(&in, CurrentTime() + 1000 * MS);
```

`GetIntuiEvent` makes no kernel call. The ring is 94 events. intuition writes nothing the program has no room for. It keeps up to 64 more, and the moves it keeps are folded into the latest. Moves take up no more than half the ring, so a program that's behind always has room for its keys and buttons. A key or a button is lost only 111 behind; `ip_Lost` counts what was dropped.

**A program that has stopped** loses nothing of its windows meanwhile. They're dragged, raised and lowered on their last frames, and the pointer moves over them, all without the program. Its events wait for it, as above.

## When a program or a server dies

- **A program that dies** loses its windows at the next latch, or the one after. intuition sees its connection go, takes its windows off, damages where they were, and lets go of their bitmaps, which are freed once nothing else maps them. A program needn't close anything before it ends.
- **intuition dying** takes the windows off the screen at the next latch: display turns off a dead client's planes, so the console shows. The program's next call returns `ERR_PORT_DIED`. A `WaitSafe` or `WaitIntuiEvent` asleep is woken to say so, as the page its sync words are on goes. The program closes its `Intuition`, opens it again with a deadline, which waits for intuition's port without polling, and opens its windows again from the bitmaps it kept, presenting each. Nothing of the program's is lost, as its pixels were always its own:
  ```c
  if (IIntuition->WaitIntuiEvent(&in, DEADLINE_NONE) == ERR_PORT_DIED) {
      IIntuition->CloseIntuition(&in);
      if (IIntuition->OpenIntuition(&in, CurrentTime() + 10000 * MS) == 0)
          reopen_windows();       /* OpenWindowTags from the same bitmaps, then PresentWindow */
  }
  ```
  A `Window` of the old connection is refused with `ERR_BADARG`, not read: its sync words were on the page that went. hwdb starts intuition again.
- **display dying** under intuition shows nothing to the program but a late `is_Disp`. intuition lets go of its buffers, keeps serving, and composes the whole screen into two new ones when display is back. Meanwhile a window of two buffers has each frame taken at once, so `WaitSafe` returns as usual, and a window of one waits for display.
- **input dying** loses nothing either. intuition opens its ring again when input is back, a little after which xhci finds the mouse and keyboard again. The pointer stays where it was, and a button held or a drag under way is let go.

intuition waits for display's and input's ports to come back with a task each, so it goes on serving its programs meanwhile.

## How intuition composes

- **Damage.** intuition keeps the rectangles of the screen that changed: a window's damage moved to where it is, or a window's whole rectangle when it opens, closes or moves. A frame is composed into the buffer not on the screen, which is a frame old, so each frame repaints what changed for it and for the last.
- **Front to back.** A rectangle to repaint is taken by the windows in depth order. The front one it meets gives the part they share, blitted from its buffer. The pieces beside that go on to the windows behind, and what none covers is the backdrop. Each pixel is written once, and the screen is never read, so it can be write-combined memory (v7-plan decision 3).
- **gfx.library's blits.** intuition composes with `IGfx->BltBitMap` and `RectFill`, by the vector unit where the CPU has one.
- **A window's parts.** A window is its inside, from the program's buffer, and four parts of chrome, from intuition's bitmaps. The front window a rectangle meets gives it from each of its parts in turn.
- **The pointer** is a 16×16 sprite on plane 2 (§13.2's `cursor.fill`), drawn at 32×32 on a screen of 1600×900 or more. Its hot spot, the tip, reaches the screen's last pixel, so at the right and bottom edges only the tip shows. Moving it is a commit of plane 2 alone, with no window composed.
- **A frame no client waits for** (the pointer moved, a window dragged, the chrome redrawn) is made just before the vblank, not as the mouse moves: committed 4 ms before it, and composing started earlier by what that should take, from the last frames' cost a pixel. A window's frame presented meanwhile goes into the same commit, not held to the latch after. A frame a client waits for is composed at once. (V7 step 6).

`make intuition-test` runs the compositor on the host with the sanitizers. Over made-up windows, with chrome and without, opened, moved, presented, raised, lowered, activated and closed at random, every frame is checked pixel for pixel against a compositor that redraws the whole screen. A watch on gfx.library's table checks that no pixel is written twice in a frame. It also tests what's where on a window's chrome, and that a slow program gets every key and button in order.

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
- **Close a window on `IDCMP_CLOSEWINDOW`**, within a second. intuition doesn't wait longer.
- **Open everything again after `ERR_PORT_DIED`.** Close the `Intuition`, open it again, and open the windows again with `OpenWindowTags`: the old `Window`s are refused.
- **Read events often.** A program that leaves them loses mouse moves first, but past 111 keys and buttons it loses those too.

## Differences from the RKM

| Intuition | intuition, V7 |
|---|---|
| `OpenWindowTags(NULL, WA_..., TAG_END)` returns a `Window *` | `OpenWindowTags(IIntuition, &in, &w, { WA_..., ... })` fills in the caller's `Window` and returns an error |
| A window draws in place, through a `RastPort` the layers clip | A window draws in bitmaps of its own, and presents frames; intuition composes them |
| Simple, smart and super bitmap refresh, and `IDCMP_REFRESHWINDOW` | No refresh: a window's last frame is always there to compose from |
| `IntuitionBase`, screens, `OpenWorkbench` | One screen, the Workbench's, reached through the caller's `Intuition` |
| A window's `UserPort`, `GetMsg` and `ReplyMsg` for IDCMP | One ring a connection, `GetIntuiEvent` copying from it, no reply |
| `MouseX`, `MouseY` from the window's corner, border and all | `iev_MouseX`, `iev_MouseY` from the inside's |
| `WA_Width`, `WA_Height` the window's outside | The inside's: the chrome is added |
| `IECODE_UP_PREFIX` (0x80) on a raw key | `RAWKEY_UP` (0x8000): HID usages run past 0x80 |
| `WFLG_REPORTMOUSE` for `IDCMP_MOUSEMOVE` | Moves come to the active window whenever it asked for them |
| Gadgets, menus, requesters | The depth and close gadgets, and the drag bar. The rest V8's, with `ui.library` |
| `WaitTOF` | `WaitSafe`, on a window's own `is_Safe` |

## Functions used

[CloseIntuition](../autodocs/intuition.md#intuitioncloseintuition), [CloseWindow](../autodocs/intuition.md#intuitionclosewindow), [GetIntuiEvent](../autodocs/intuition.md#intuitiongetintuievent), [OpenIntuition](../autodocs/intuition.md#intuitionopenintuition), [OpenWindowTagList](../autodocs/intuition.md#intuitionopenwindowtaglist), [PresentWindow](../autodocs/intuition.md#intuitionpresentwindow), [WaitIntuiEvent](../autodocs/intuition.md#intuitionwaitintuievent), [WaitSafe](../autodocs/intuition.md#intuitionwaitsafe); [AllocBitMap](../autodocs/gfx.md#gfxallocbitmap), [BltBitMap](../autodocs/gfx.md#gfxbltbitmap), [ShareArea](../autodocs/exec.md#execsharearea), [WaitAddr](../autodocs/exec.md#execwaitaddr).
