# intuition: windows on the screen

*Checked against V8 step 4; settings and backdrop windows, V8 step 7.*

intuition.library's functions, in alphabetical order. The header is `planar/intuition.h`. They're a shared library's, called through `IIntuition` (`struct IntuitionIFace`, the "main" interface, version 1, of intuition.library 8; a program opening version 7 gets the same table, V8 step 4's four calls and step 7's two at its end). Each works on the caller's own `Intuition`, the connection to the intuition server, or a `Window` on it. They keep no state of their own but a page per connection, the caller's, which says how far it has read its events. One task at a time may use an `Intuition` and its windows. [Libraries ch. 13](../libraries/13-intuition.md) is the guide to them.

---

## intuition/ChangeWindowBuffers

**NAME**
ChangeWindowBuffers -- give a resized window buffers of its new size

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t ChangeWindowBuffers(struct Window *w, struct BitMap *bm, struct BitMap *back);
```

**FUNCTION**
After `IDCMP_NEWSIZE`, gives the window new buffers, at least the size its sync words say (`is_Width` by `is_Height`). Shares them with intuition, read-only, and waits a second at most for its answer. Then `w_Width` and `w_Height` are the new size, `w_BitMap` the new buffers, `w_Back` the first, and `w_RPort` on it: draw the next frame there, and present it. intuition takes the new buffers at that frame, showing the old one until then, and lets go of its mappings of the old ones as it does.

**INPUTS**
- `w` -- an open window.
- `bm` -- the first new buffer, `AllocBitMap`'s.
- `back` -- a second, or `nullptr` for one.

**RESULT**
0, or:
- `ERR_BADARG` -- the window isn't open, a bitmap isn't `AllocBitMap`'s or is smaller than the size, or intuition says the size isn't the window's (it was resized again meanwhile: wait for that `IDCMP_NEWSIZE`).
- `ERR_RIGHTS` -- a bitmap's memory isn't shared with intuition.
- `ERR_NOTFOUND` -- intuition took the window off the screen.
- `ERR_TIMEOUT`, `ERR_PORT_DIED` -- intuition didn't answer in time, or has gone.

**EXAMPLE**
From `conformance/window.c`:
```c
struct BitMap *fbm = IGfx->AllocBitMap(240, 150, PIXFMT_XRGB8888, 0, nullptr);

if (IIntuition->ChangeWindowBuffers(&we, fbm, nullptr) == 0) {
    picture(&we.w_RPort, 240, 150, 0x3a6298, "resized");
    IIntuition->PresentWindow(&we, nullptr, 0);
}
```

**NOTES**
- The old buffers may be freed once this returns: intuition's mapping keeps their memory until it lets go of them.
- A buffer bigger than the window is shown as far as the window goes, so a program may keep one big buffer through many resizes, and call this only to grow it.
- Two calls before a frame: the second's buffers are the ones taken.
- **Differs from the RKM:** the RKM's window was resized in place, its layer's bitmap grown by the system. Planar's buffers are the program's.

**BUGS**
None known.

**SEE ALSO**
[OpenWindowTagList](#intuitionopenwindowtaglist), [PresentWindow](#intuitionpresentwindow), [GetIntuiEvent](#intuitiongetintuievent).

---

## intuition/ClearMenuStrip

**NAME**
ClearMenuStrip -- take a window's menus away

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t ClearMenuStrip(struct Window *w);
```

**FUNCTION**
`SetMenuStrip(w, nullptr)`: the window has no menus, and the screen bar shows none when it's active. A menu of it open is closed.

**INPUTS**
- `w` -- an open window.

**RESULT**
As `SetMenuStrip`.

**NOTES**
- **Differs from the RKM:** not needed before changing a strip, as intuition keeps a copy, and `SetMenuStrip` replaces it whole.

**BUGS**
None known.

**SEE ALSO**
[SetMenuStrip](#intuitionsetmenustrip).

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
Closes the connection `OpenIntuition` made. intuition closes every window still open on it, as it does when a client dies. The connection's page of events goes with it, and the caller's mapping of it is let go of here, in case intuition is dead and can't take it back.

**INPUTS**
- `in` -- an `Intuition` that `OpenIntuition` opened.

**RESULT**
None.

**NOTES**
- The windows' bitmaps stay the caller's to free. intuition lets go of its mappings of them as it closes the windows.
- After `ERR_PORT_DIED`, this and then `OpenIntuition` again make a new connection to intuition's next self. The old `Window`s are refused (`ERR_BADARG`) and must be opened again (Libraries ch. 13, "When a program or a server dies").

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

A window whose close gadget was clicked must be closed within a second of its `IDCMP_CLOSEWINDOW`. If it isn't, intuition asks the user whether to close it (V8 step 4), and on *Close* asks hwdb to end the program, which takes its windows with it. If it can't ask, or hwdb won't, it takes the window off the screen and lets go of its bitmaps, and says so in the log (v7-plan decision 6). Its `is_Safe` goes to its highest, so `WaitSafe` returns, and its next `PresentWindow` is refused with `ERR_NOTFOUND`. `CloseWindow` on it still returns 0.

**INPUTS**
- `w` -- a window `OpenWindowTagList` opened.

**RESULT**
0, or:
- `ERR_BADARG` -- `w` isn't open, or was opened on a connection since closed.
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

## intuition/EasyRequest

**NAME**
EasyRequest -- ask the user, and wait for the answer

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t EasyRequest(struct Intuition *in, struct Window *w, const struct EasyStruct *es);
```

**FUNCTION**
Shows a requester, a small window intuition draws and owns: `es_Title` in its title bar, `es_TextFormat`'s lines (split at `'\n'`), and a button for each of `es_GadgetFormat`'s labels (split at `'|'`), along the bottom. It's centred over `w`, or the screen. Waits for the user to click a button, however long that takes, then the requester goes.

**INPUTS**
- `in` -- an open `Intuition`, used by nothing else until this returns.
- `w` -- the window it's about, of the same connection, or `nullptr`.
- `es` -- `es_Title` (31 bytes at most), `es_TextFormat` (479, 12 lines), `es_GadgetFormat` (95, 6 buttons, at least one). `es_StructSize` and `es_Flags` aren't read.

**RESULT**
The button clicked: 1 for the leftmost, 2 for the next and so on, and 0 for the rightmost (the RKM's numbering, where the rightmost is "Cancel"). Or:
- `ERR_BADARG` -- no buttons, text too long, or `w` not open on `in`.
- `ERR_FULL` -- 4 requesters are up already.
- `ERR_NOMEM` -- intuition had no memory for it.
- `ERR_PORT_DIED` -- intuition went.

**EXAMPLE**
From `conformance/window.c`, on a task and connection of its own:
```c
const struct EasyStruct es = { sizeof es, 0, "A question", "Which one?\nThe middle, please.",
                               "Left|Middle|Right" };

answer = IIntuition->EasyRequest(&easy.in, nullptr, &es);      /* 2: Middle */
```

**NOTES**
- **Differs from the RKM:** `EasyRequestArgs` took `printf` arguments and an IDCMP pointer that could end it; this takes the text as it is, and only a button ends it. Its window's input isn't blocked: other windows, and the one it's about, go on.
- The answer comes on the connection, so nothing else may call through `in` meanwhile, from any task: a program that goes on drawing opens a second `Intuition` to ask on.
- The requester takes no keys. It can be raised and lowered as any window, and isn't dragged.

**BUGS**
None known.

**SEE ALSO**
[SetMenuStrip](#intuitionsetmenustrip), [OpenIntuition](#intuitionopenintuition).

---

## intuition/GetIntuiEvent

**NAME**
GetIntuiEvent -- take the next event of the caller's windows

**SYNOPSIS**
```c
#include <planar/intuition.h>
bool GetIntuiEvent(struct Intuition *in, struct IntuiEvent *ev);
```

**FUNCTION**
Copies the oldest event not yet taken from the connection's ring into `ev`, and returns true. Returns false at once if there's none. Events come for every window open on the connection, in the order intuition sent them, and each says its window's `w_Id` in `iev_Window`.

An event is one of the IDCMP classes its window asked for with `WA_IDCMP`:
- `IDCMP_MOUSEBUTTONS` -- a button pressed over the window's inside, `iev_Code` `SELECTDOWN`, `MENUDOWN` or `MIDDLEDOWN`; and let go, wherever the pointer is then, `SELECTUP`, `MENUUP` or `MIDDLEUP`.
- `IDCMP_MOUSEMOVE` -- the pointer moved while the window was the active one, anywhere on the screen.
- `IDCMP_RAWKEY` -- a key at the active window: `iev_Code` its HID usage (planar/devices/input.h), with `RAWKEY_UP` when it's let go.
- `IDCMP_VANILLAKEY` -- a key at the active window that types a character, going down: `iev_Code` the character. With both asked for, such a key comes as `IDCMP_VANILLAKEY`, and its going up as `IDCMP_RAWKEY`.
- `IDCMP_ACTIVEWINDOW`, `IDCMP_INACTIVEWINDOW` -- the window became the active one, or stopped being.
- `IDCMP_CLOSEWINDOW` -- its close gadget was clicked. Close the window with `CloseWindow`, within a second.
- `IDCMP_MENUPICK` -- an item of its menu strip was chosen ([SetMenuStrip](#intuitionsetmenustrip)): `iev_Code` the RKM's packed number (`MENUNUM`, `ITEMNUM`; `SUBNUM` is `NOSUB`), and `iev_Char` 1 if a `CHECKIT` item is checked now, 0 if not.
- `IDCMP_NEWSIZE` -- the window was resized by its size gadget: `iev_MouseX` and `iev_MouseY` are its inside's new width and height, as its sync words' `is_Width` and `is_Height` are. Give it buffers of that size with [ChangeWindowBuffers](#intuitionchangewindowbuffers).
- `IDCMP_NEWPREFS` -- intuition's settings changed (V8 step 7): read them with [GetPrefs](#intuitiongetprefs).

Otherwise `iev_MouseX` and `iev_MouseY` are where the pointer was, in the window's inside's coordinates: (0, 0) is the top left pixel of its buffers. `iev_Qualifier` is input's `IEQ_*` (the modifier keys), with `IEQ_LEFTBUTTON`, `IEQ_RBUTTON` and `IEQ_MIDBUTTON` for the buttons held and `IEQ_REPEAT` for a key's repeat. `iev_Time` is when input had it, `CurrentTime()`'s.

**INPUTS**
- `in` -- an open `Intuition`.
- `ev` -- filled in by this, if it returns true.

**RESULT**
True if there was an event.

**EXAMPLE**
From `conformance/window.c`, shortened:
```c
struct IntuiEvent ev;

for (;;) {
    while (IIntuition->GetIntuiEvent(&in, &ev))
        if (ev.iev_Class == IDCMP_CLOSEWINDOW && ev.iev_Window == w.w_Id)
            IIntuition->CloseWindow(&w);
    if (IIntuition->WaitIntuiEvent(&in, CurrentTime() + 2000000000) != 0)
        break;
}
```

**NOTES**
- It makes no kernel call, except when it takes an event from a ring that was full, or held `INTUI_EVENTS_MOVES` (47): then intuition may be holding events back for room, and is told with `SignalConn`.
- **A slow reader loses moves first.** intuition writes no event to the ring that the caller has no room for, and keeps up to 64 more. Mouse moves take up no more than 47 of the ring's 94, and a move kept waiting is replaced by the next. Past what it keeps, intuition drops the oldest move, and if it holds none, the event that came. So a key or a button is lost only 111 of them behind. `in_Page->ip_Lost` counts what was dropped.
- **Differs from the RKM:** there's no `UserPort` to `GetMsg` from, and nothing to `ReplyMsg`. A connection's windows share one ring, and an event is copied out of it. Coordinates are the inside's, not the window's with its border.

**BUGS**
None known.

**SEE ALSO**
[WaitIntuiEvent](#intuitionwaitintuievent), [OpenWindowTagList](#intuitionopenwindowtaglist), [CloseWindow](#intuitionclosewindow).

---

## intuition/GetPrefs

**NAME**
GetPrefs, SetPrefs -- intuition's settings, read and set (V8 step 7)

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t GetPrefs(struct Intuition *in, struct IntuiPrefs *prefs);
int64_t SetPrefs(struct Intuition *in, const struct IntuiPrefs *prefs);
```

**FUNCTION**
`GetPrefs` copies intuition's settings into `prefs`. `SetPrefs` puts all of them in force at once, and tells every window that asked for `IDCMP_NEWPREFS`:
- `ip_Hud` -- 1: the Workbench shows its CPU and memory badge.
- `ip_PointerSize` -- `INTUI_POINTER_AUTO` (twice the size on a screen of 1600 by 900 or more), `INTUI_POINTER_SMALL` or `INTUI_POINTER_LARGE`. intuition's own: the sprite shown changes at once.
- `ip_BackdropTop`, `ip_BackdropBottom` -- the Workbench's backdrop gradient, XRGB: §13.2's `INTUI_BACKDROP_TOP` and `INTUI_BACKDROP_BOTTOM` by default.

intuition reads its saved settings when it starts (`SYS:Prefs/Env-Archive/intuition.prefs`, [ReadPrefs](dos.md#dosreadprefs)): at once if dos is there, else once it is, unless a client has set them first.

**INPUTS**
- `in` -- an open `Intuition`.
- `prefs` -- the settings: every field, each in its range.

**RESULT**
0, `ERR_BADARG` (`SetPrefs`: a field out of its range: `ip_Hud` past 1, `ip_PointerSize` past 2, a colour past `0xffffff`; nothing changes), or `ERR_TIMEOUT` and `ERR_PORT_DIED` as every call.

**EXAMPLE**
From `servers/workbench/main.c`, on `IDCMP_NEWPREFS`:
```c
if (wb.intuition->GetPrefs(&wb.in, &p) != 0)
    return;
hud = p.ip_Hud != wb.prefs.ip_Hud;
```

**NOTES**
- The RKM's `GetPrefs` and `SetPrefs` copied a `struct Preferences` of everything; these are intuition's few, and input's settings are input's (`INPUT_GET`, [Devices ch. 7](../devices/07-input.md)).

**BUGS**
None known.

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
Connects to the "intuition" port, waiting until `deadline` for it to be published, and asks for the screen's size and intuition's task. Then it makes the connection's `IntuiAck` page, which says how far the caller has read its events, shares it with intuition read-only, and gets the connection's own page from intuition, mapped read-only: the windows' sync words, and the ring of events. Fills in `in`.

**INPUTS**
- `in` -- the caller's `Intuition`, filled in by this.
- `deadline` -- `CurrentTime()`'s clock. 0 doesn't wait: if intuition isn't running, it's `ERR_NOTFOUND` at once.

**RESULT**
0, or:
- `ERR_TIMEOUT` -- intuition wasn't published by the deadline.
- `ERR_NOTFOUND` -- it isn't, with a deadline of 0.
- `ERR_PORT_DIED` -- it went before it answered.
- `ERR_NOMEM` -- the connection or its pages couldn't be made.
- What `AllocArea` or `ShareArea` returned, if it failed.

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
- With a deadline, it's the way back after intuition dies: hwdb starts it again, and this connects as soon as it's published.

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

The window's inside is the top left `WA_Width` by `WA_Height` of the bitmap presented. Draw a frame through `w->w_RPort`, which is on `w->w_BitMap[w->w_Back]`. Around the inside intuition draws the window's chrome (§13.3), unless it's `WFLG_BORDERLESS`: a title bar of `INTUI_TITLEBAR` (26) pixels above, with the title in System Sans and the gadgets asked for, and a frame of `INTUI_BORDER` (4) on the other sides, or `INTUI_SIZEBORDER` (16) at the bottom with a size gadget. `w->w_BorderLeft`, `w_BorderTop`, `w_BorderRight` and `w_BorderBottom` say where the inside is in the window, from the chrome's top left.

**INPUTS**
- `in` -- an open `Intuition`.
- `w` -- the caller's `Window`, filled in by this.
- `tags`:
  - `WA_BitMap` -- `struct BitMap *`: the first buffer. Needed. It must be `AllocBitMap`'s, an Area of the caller's that can be shared.
  - `WA_BackBitMap` -- `struct BitMap *`: a second buffer, to draw a frame in while the last is shown. None by default.
  - `WA_Left`, `WA_Top` -- where the window's top left goes on the screen, its chrome's. 0 by default. Either may be negative, or past the screen's edge, by up to 16384.
  - `WA_Width`, `WA_Height` -- the inside's size, no larger than either bitmap's. The first bitmap's by default.
  - `WA_Title` -- `const char *`: the title bar's, 31 bytes at most, cut short where the gadgets begin. "" by default.
  - `WA_Flags` -- `WFLG_*`, the RKM's: `WFLG_DRAGBAR` (dragged by its title bar), `WFLG_DEPTHGADGET`, `WFLG_CLOSEGADGET`, `WFLG_SIZEGADGET` (resized by its bottom right corner: `IDCMP_NEWSIZE`), `WFLG_BORDERLESS` (no chrome: the inside alone, as in V7 step 3), `WFLG_ACTIVATE` (the active window at its first frame), `WFLG_BACKDROP` (V8 step 7: behind every other window always, and not raised by a click: the Workbench's). None by default: a title bar and a frame, with no gadgets, that can't be dragged.
  - `WA_MinWidth`, `WA_MinHeight`, `WA_MaxWidth`, `WA_MaxHeight` -- the inside's size a resize is held to. 1, 1, and the screen's by default (0 is the screen's).
  - `WA_IDCMP` -- the IDCMP classes the window's events are wanted for ([GetIntuiEvent](#intuitiongetintuievent)). None by default.

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
- **Differs from the RKM:** the caller provides the `Window` and the bitmaps, and the result is an error, not a `Window *`. There's no `NewWindow`. Of `WA_Flags`, only the seven above; iconify, zoom and the border scrollers aren't built (v8-plan decision 9). The window's size is its inside's, not its outside's, and so are its limits.
- A window's top left may be under the screen bar, which is shown over it; a drag keeps its title bar below the bar.
- The window is opaque in V7: an ARGB bitmap's alpha is ignored.
- intuition describes each buffer itself, from what this sends: the address, the row stride, the width and the height, checked against the Area holding them. The `BitMap` in the Area isn't read.
- A bitmap shared before keeps its mapping (`ERR_EXISTS` from `ShareArea` is taken as done). One that intuition refuses is let go again, unless another window uses it.

**BUGS**
- A client that unshares a bitmap from intuition while its window is open ends intuition, at the next frame that reads it (`UnshareArea`'s note). V7 step 5's supervision restarts it. So does one that unshares its `IntuiAck`'s page, at the next event.
- intuition checks that a buffer is memory another process shared with it, not that it's the caller's: a client that knows the address of another's bitmap can show it in a window of its own. `QueryArea` doesn't say whose an Area is.

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
- `damage` -- rectangles in the window's inside's coordinates, inclusive, each inside the window (the library's check) and its buffer (intuition's: after a resize a window and its buffer may differ). Not read if `count` is 0.
- `count` -- 0 to `INTUI_RECTS` (16). 0 is the whole window.

**RESULT**
0, or:
- `ERR_BADARG` -- the window isn't open (or was opened on a connection since closed), there are too many rectangles, or one isn't inside it.
- `ERR_TIMEOUT`, `ERR_PORT_DIED` -- the last `INTUI_INFLIGHT` (4) presents are all unanswered, a second on, or intuition has gone.
- `ERR_NOTFOUND`, from intuition, later -- it took the window off the screen, as its close gadget went unanswered.
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

## intuition/SetMenuStrip

**NAME**
SetMenuStrip -- give a window its menus

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t SetMenuStrip(struct Window *w, const struct NewMenu *menus);
```

**FUNCTION**
Gives the window a menu strip, made from `menus`, an array of GadTools' `NewMenu`s ended by `NM_END`: each `NM_TITLE` a menu, and the `NM_ITEM`s after it its items. intuition keeps a copy, and while the window is active the screen bar shows its titles. intuition opens, draws and tracks the menus itself, and a choice comes as `IDCMP_MENUPICK` (Libraries ch. 13, *Menus*). Replaces any strip the window had; `nullptr` or an empty array takes it away.

**INPUTS**
- `w` -- an open window.
- `menus` -- the strip:
  - `nm_Type` -- `NM_TITLE`, `NM_ITEM`, or `NM_END`.
  - `nm_Label` -- the label, 23 bytes at most; for an item, `NM_BARLABEL` is a separator, which can't be chosen but is counted in `ITEMNUM`.
  - `nm_CommKey` -- an item's key, chosen with the GUI key held: its first character, letters as capitals. `nullptr` for none.
  - `nm_Flags` -- a title's `NM_MENUDISABLED`; an item's `NM_ITEMDISABLED`, `CHECKIT` (checked and unchecked by choosing it) and `CHECKED` (checked to begin with).

**RESULT**
0, or:
- `ERR_BADARG` -- the window isn't open; more than 8 menus, 64 items in all, or 63 in a menu; a label too long; an item before any title.
- `ERR_NOTFOUND` -- intuition took the window off the screen.
- `ERR_TIMEOUT`, `ERR_PORT_DIED` -- intuition didn't answer in time, or has gone.

**EXAMPLE**
From `conformance/window.c`:
```c
static const struct NewMenu strip[] = {
    { NM_TITLE, "Project", nullptr, 0 },
    { NM_ITEM, "Open", nullptr, 0 },
    { NM_ITEM, "Save", "s", 0 },
    { NM_ITEM, NM_BARLABEL, nullptr, 0 },
    { NM_ITEM, "Quit", "Q", 0 },
    { NM_TITLE, "Options", nullptr, 0 },
    { NM_ITEM, "Snap to grid", nullptr, CHECKIT },
    { NM_END, nullptr, nullptr, 0 },
};

IIntuition->SetMenuStrip(&we, strip);
```

**NOTES**
- **Differs from the RKM:** it takes GadTools' `NewMenu` array, not a linked `Menu` strip from `CreateMenus` and `LayoutMenus`; intuition lays it out. There are no sub-items, no `MutualExclude`, no images, and no `NextSelect`: one `IDCMP_MENUPICK` an item.
- The strip travels in the page the connection's `IntuiAck` is on, which intuition copies during the call.
- A `CHECKIT` item's state is intuition's after this: it toggles it at each choice, and says which it is in `iev_Char`. `SetMenuStrip` again sets it afresh.

**BUGS**
None known.

**SEE ALSO**
[ClearMenuStrip](#intuitionclearmenustrip), [GetIntuiEvent](#intuitiongetintuievent), [EasyRequest](#intuitioneasyrequest).

---

## intuition/WaitIntuiEvent

**NAME**
WaitIntuiEvent -- wait for an event of the caller's windows

**SYNOPSIS**
```c
#include <planar/intuition.h>
int64_t WaitIntuiEvent(struct Intuition *in, uint64_t deadline);
```

**FUNCTION**
Returns once the connection's ring has an event not yet taken: `GetIntuiEvent` then returns it. If one is there already, it returns at once with no kernel call. Otherwise it sleeps in `WaitAddr` on the ring's `ip_Head`.

**INPUTS**
- `in` -- an open `Intuition`.
- `deadline` -- `CurrentTime()`'s clock: `DEADLINE_NONE` for none, and 0 not to wait.

**RESULT**
0, or:
- `ERR_BADARG` -- `in` isn't open.
- `ERR_TIMEOUT` -- the deadline passed first.
- `ERR_PORT_DIED` -- intuition has gone. A wait under way when it dies returns this at once: the page is woken as intuition's mapping of it goes.

**NOTES**
- **Differs from the RKM:** `Wait` on a window's `UserPort` signal. A program waiting for events and its frames both waits on one, and polls the other: see `WaitSafe`'s no-call return.

**BUGS**
None known.

**SEE ALSO**
[GetIntuiEvent](#intuitiongetintuievent), [WaitSafe](#intuitionwaitsafe), [WaitAddr](exec.md#execwaitaddr).

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
- `deadline` -- `CurrentTime()`'s clock: `DEADLINE_NONE` for none, and 0 not to wait.

**RESULT**
0, or:
- `ERR_BADARG` -- the window isn't open, or was opened on a connection since closed.
- `ERR_TIMEOUT` -- the deadline passed first.
- `ERR_PORT_DIED` -- intuition has gone, as for `WaitIntuiEvent`. While display is away, a window of two buffers is still made safe at once; one of one waits for display.
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
  - `is_DispTime` -- when it latched, `CurrentTime()`'s;
  - `is_LeftEdge`, `is_TopEdge` -- where the window is, its chrome's top left: where it opened, or where it was dragged to.
- With one buffer, intuition may compose from it again while the next frame is drawn, when something else on the screen changes there. That part of the window may then show a frame half drawn, until it's presented.
- **Planar's addition.** §16.3's pillar 4, as display's `ds_Safe` is for a plane's buffers.

**BUGS**
None known.

**SEE ALSO**
[PresentWindow](#intuitionpresentwindow), [WaitAddr](exec.md#execwaitaddr).
