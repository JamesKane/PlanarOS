# intuition: windows on the screen

*Checked against V7 step 3.*

intuition.library's functions, in alphabetical order. The header is `planar/intuition.h`. They're a shared library's, called through `IIntuition` (`struct IntuitionIFace`, the "main" interface, version 1, of intuition.library 7). Each works on the caller's own `Intuition`, the connection to the intuition server, or a `Window` on it. They keep no state of their own. One task at a time may use an `Intuition` and its windows. [Libraries ch. 13](../libraries/13-intuition.md) is the guide to them.

---

## intuition/CloseIntuition

**NAME**
CloseIntuition -- close the connection to intuition

**SYNOPSIS**
```c
#include <planar/intuition.h>
void CloseIntuition(struct Intuition *in);
```

**FUNCTION**
Closes the connection `OpenIntuition` made. intuition closes every window still open on it, as it does when a client dies.

**INPUTS**
- `in` -- an `Intuition` that `OpenIntuition` opened.

**RESULT**
None.

**NOTES**
- The windows' bitmaps stay the caller's to free. intuition lets go of its mappings of them as it closes the windows.

**BUGS**
None known.

**SEE ALSO**
[OpenIntuition](#intuitionopenintuition), [CloseWindow](#intuitionclosewindow).

---

## intuition/CloseWindow

**NAME**
CloseWindow -- take a window off the screen

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t CloseWindow(struct Window *w);
```

**FUNCTION**
Asks intuition to close the window, and waits a second at most for the answer. What the window covered is the backdrop, or the windows behind it, from the next frame. intuition lets go of its mappings of the window's bitmaps, unless another window of the caller's uses them.

**INPUTS**
- `w` -- a window `OpenWindowTagList` opened.

**RESULT**
0, or:
- `ERR_BADARG` -- `w` isn't open.
- `ERR_NOTFOUND` -- intuition had no such window of the caller's.
- `ERR_TIMEOUT`, `ERR_PORT_DIED` -- intuition didn't answer in time, or has gone.

Either way, `w` is closed afterwards.

**NOTES**
- **Differs from the RKM:** the `Window` is the caller's, and so are its bitmaps, which stay allocated: free them with `FreeBitMap` after.

**BUGS**
None known.

**SEE ALSO**
[OpenWindowTagList](#intuitionopenwindowtaglist), [CloseIntuition](#intuitioncloseintuition).

---

## intuition/OpenIntuition

**NAME**
OpenIntuition -- connect to intuition

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t OpenIntuition(struct Intuition *in, uint64_t deadline);
```

**FUNCTION**
Connects to the "intuition" port, waiting until `deadline` for it to be published, and asks for the screen's size and intuition's task. Fills in `in`.

**INPUTS**
- `in` -- the caller's `Intuition`, filled in by this.
- `deadline` -- `CurrentTime()`'s clock. 0 doesn't wait: if intuition isn't running, it's `ERR_NOTFOUND` at once.

**RESULT**
0, or:
- `ERR_TIMEOUT` -- intuition wasn't published by the deadline.
- `ERR_NOTFOUND` -- it isn't, with a deadline of 0.
- `ERR_PORT_DIED` -- it went before it answered.
- `ERR_NOMEM` -- the connection couldn't be made.

**EXAMPLE**
```c
struct Intuition in;

if (IIntuition->OpenIntuition(&in, CurrentTime() + 5000000000) != 0)
    return 1;
```

**NOTES**
- `in_Width` and `in_Height` are the screen's. A window may be anywhere, on it or off it.
- **Differs from the RKM:** there's no `IntuitionBase` to open, nor `OpenWorkbench`. The Workbench's screen is the only one, and the `Intuition` is the caller's connection to it.
- The connection uses signal 22 of the calling task.

**BUGS**
None known.

**SEE ALSO**
[CloseIntuition](#intuitioncloseintuition), [OpenWindowTagList](#intuitionopenwindowtaglist), [ConnectPortUntil](msg.md#execconnectport).

---

## intuition/OpenWindowTagList

**NAME**
OpenWindowTagList, OpenWindowTags -- open a window on the screen

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t OpenWindowTagList(struct Intuition *in, struct Window *w, const struct TagItem *tags);
int64_t OpenWindowTags(iface, in, w, { tag, data }, ...);     /* a macro */
```

**FUNCTION**
Shares the window's bitmaps with intuition, read-only, and asks it to open the window. The window isn't on the screen until its first `PresentWindow`. Then it comes to the front.

The window's pixels are the top left `WA_Width` by `WA_Height` of the bitmap presented. Draw a frame through `w->w_RPort`, which is on `w->w_BitMap[w->w_Back]`.

**INPUTS**
- `in` -- an open `Intuition`.
- `w` -- the caller's `Window`, filled in by this.
- `tags`:
  - `WA_BitMap` -- `struct BitMap *`: the first buffer. Needed. It must be `AllocBitMap`'s, an Area of the caller's that can be shared.
  - `WA_BackBitMap` -- `struct BitMap *`: a second buffer, to draw a frame in while the last is shown. None by default.
  - `WA_Left`, `WA_Top` -- where the window's top left goes on the screen. 0 by default. Either may be negative, or past the screen's edge.
  - `WA_Width`, `WA_Height` -- the window's size, no larger than either bitmap's. The first bitmap's by default.
  - `WA_Title` -- `const char *`: kept for V7 step 4's title bar, 31 bytes at most. "" by default.

**RESULT**
0, or:
- `ERR_BADARG` -- no `WA_BitMap`, a bitmap that isn't `AllocBitMap`'s, a window bigger than a bitmap, or a bitmap whose rows don't all lie in its Area.
- `ERR_RIGHTS` -- a bitmap's memory isn't shared with intuition.
- `ERR_FULL` -- the screen has 32 windows already, or the connection has 16.
- `ERR_TIMEOUT`, `ERR_PORT_DIED` -- intuition didn't answer in time, or has gone.
- What `ShareArea` returned, if it failed.

**EXAMPLE**
From `conformance/window.c`:
```c
struct BitMap *front = IGfx->AllocBitMap(200, 120, PIXFMT_ARGB8888, 0, nullptr);
struct BitMap *back = IGfx->AllocBitMap(200, 120, PIXFMT_ARGB8888, 0, nullptr);
struct Window w;

if (OpenWindowTags(IIntuition, &in, &w, { WA_Left, 64 }, { WA_Top, 48 },
                   { WA_BitMap, (uint64_t)front }, { WA_BackBitMap, (uint64_t)back }) != 0)
    return 1;
```

**NOTES**
- **Differs from the RKM:** the caller provides the `Window` and the bitmaps, and the result is an error, not a `Window *`. There's no `NewWindow`, and none of `WA_Flags`' gadgets until V7 step 4's chrome.
- The window is opaque in V7: an ARGB bitmap's alpha is ignored.
- intuition describes each buffer itself, from what this sends: the address, the row stride, the width and the height, checked against the Area holding them. The `BitMap` in the Area isn't read.
- A bitmap shared before keeps its mapping (`ERR_EXISTS` from `ShareArea` is taken as done). One that intuition refuses is let go again, unless another window uses it.

**BUGS**
- A client that unshares a bitmap from intuition while its window is open ends intuition, at the next frame that reads it (`UnshareArea`'s note). V7 step 5's supervision restarts it.

**SEE ALSO**
[PresentWindow](#intuitionpresentwindow), [CloseWindow](#intuitionclosewindow), [AllocBitMap](gfx.md#gfxallocbitmap), [ShareArea](exec.md#execsharearea), [GetTagData](utility.md#utilitygettagdata).

---

## intuition/PresentWindow

**NAME**
PresentWindow -- show the frame just drawn

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t PresentWindow(struct Window *w, const struct GfxRect *damage, uint32_t count);
```

**FUNCTION**
Tells intuition that the next frame is in `w_BitMap[w_Back]`, with `damage` the rectangles of it that changed since the last frame presented. Then it moves `w_Back` to the other buffer, if there are two, and `w_RPort` with it. `w_Frame` counts the frames presented.

It doesn't wait for intuition: the message goes, and its answer is taken later. intuition composes the window's latest frame at the next frame it composes, one a vblank at most. When it has, the window's `is_Safe` is the frame's number, and its `is_Disp` once the frame is on the screen.

**INPUTS**
- `w` -- an open window.
- `damage` -- rectangles in the window's own coordinates, inclusive, each inside the window. Not read if `count` is 0.
- `count` -- 0 to `INTUI_RECTS` (16). 0 is the whole window.

**RESULT**
0, or:
- `ERR_BADARG` -- the window isn't open, there are too many rectangles, or one isn't inside it.
- `ERR_TIMEOUT`, `ERR_PORT_DIED` -- the last `INTUI_INFLIGHT` (4) presents are all unanswered, a second on, or intuition has gone.
- An earlier present's refusal, from intuition, which this is the first to say. The frame was presented.

**EXAMPLE**
```c
static const struct GfxRect bar = { 16, 40, 111, 55 };

IGfx->RectFill(&w.w_RPort, bar.MinX, bar.MinY, bar.MaxX, bar.MaxY);
IIntuition->PresentWindow(&w, &bar, 1);
IIntuition->WaitSafe(&w, CurrentTime() + 1000000000);
```

**NOTES**
- **Damage is from the last frame**, not from what the buffer held before. With two buffers, bring the back one up to date with the last frame's changes before drawing the next: blit them across, or redraw both frames' damage.
- Its only kernel call, usually, is the signal that wakes intuition if it's asleep.
- **Planar's addition:** the RKM had no presentation. A window there was drawn in place, under the layers' clips.

**BUGS**
None known.

**SEE ALSO**
[WaitSafe](#intuitionwaitsafe), [OpenWindowTagList](#intuitionopenwindowtaglist), [PutMsg](msg.md#execputmsg).

---

## intuition/WaitSafe

**NAME**
WaitSafe -- wait until the back buffer may be drawn in

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t WaitSafe(struct Window *w, uint64_t deadline);
```

**FUNCTION**
Returns once intuition has composed the last frame presented, which its `is_Safe` says. intuition then reads nothing of the window's other buffer, so the next frame may be drawn there. A window with one buffer may draw in it again.

If `is_Safe` is there already, it returns at once with no kernel call. Otherwise it sleeps in `WaitAddr` on `is_Safe`.

**INPUTS**
- `w` -- an open window.
- `deadline` -- `CurrentTime()`'s clock. 0 for none.

**RESULT**
0, or:
- `ERR_BADARG` -- the window isn't open.
- `ERR_TIMEOUT` -- the deadline passed first.
- `ERR_PORT_DIED` -- intuition has gone.
- An earlier present's refusal, from intuition.

**EXAMPLE**
```c
for (;;) {
    draw(&w.w_RPort);
    IIntuition->PresentWindow(&w, nullptr, 0);
    if (IIntuition->WaitSafe(&w, CurrentTime() + 1000000000) != 0)
        break;
}
```

**NOTES**
- `w->w_Sync`'s words are a cache line of a page intuition shares read-only:
  - `is_Safe` -- the last frame composed;
  - `is_Disp` -- the last frame on the screen;
  - `is_DispTime` -- when it latched, `CurrentTime()`'s.
- With one buffer, intuition may compose from it again while the next frame is drawn, when something else on the screen changes there. That part of the window may then show a frame half drawn, until it's presented.
- **Planar's addition.** §16.3's pillar 4, as display's `ds_Safe` is for a plane's buffers.

**BUGS**
None known.

**SEE ALSO**
[PresentWindow](#intuitionpresentwindow), [WaitAddr](exec.md#execwaitaddr).
