# ui: gadgets in the caller's window

*Checked against V8 step 6; the backfill, step 7.*

ui.library's functions, in alphabetical order. The header is `planar/ui.h`. They're a shared library's, called through `IUI` (`struct UIIFace`, the "main" interface, version 1, of ui.library 8). The same functions by the same names are in `lib/ui`, for the host's test. The library keeps nothing of its own: a `struct UI` is the caller's, gadgets and all, and it draws through the caller's gfx.library. One task uses a UI at a time.

[Libraries chapter 15](../libraries/15-ui.md) says how the calls go together.

---

## ui/AddGadget

**NAME**
AddGadget -- a gadget, the last of its group's

**SYNOPSIS**
```c
#include <planar/ui.h>
uint32_t AddGadget(struct UI *ui, uint32_t parent, uint32_t kind, const struct TagItem *tags);
#define AddGadgetTags(iface, ui, parent, kind, ...)
```

**FUNCTION**
Adds a gadget of `kind` to the UI, after `parent`'s other children, with the attributes `tags` gives. Gadgets are numbered from 1 in the order they're added, and the first is the root, laid out over the whole inside. A gadget's strings, a list's or a selector's items, and a text field's buffer are the caller's, and must outlive the gadget.

A split pane's first child is drawn on the dark pane, and its second on the light one, unless they say otherwise (`UIA_Pane`).

**INPUTS**
- `ui` -- from [InitUI](#uiinitui).
- `parent` -- a group's number (`UI_ROW`, `UI_COLUMN`, `UI_SPLITPANE`); 0 for the root, which must be the first gadget and is the only one with no parent.
- `kind` -- `UI_ROW`, `UI_COLUMN`, `UI_SPLITPANE`, `UI_DIVIDER`, `UI_TOGGLE`, `UI_SLIDER`, `UI_SEGMENTED`, `UI_HUDBADGE`, `UI_BUTTON`, `UI_LABEL`, `UI_LIST` or `UI_TEXTFIELD`.
- `tags` -- `UIA_*`, ended by `TAG_END`:
  - `UIA_Label` -- a label's, a button's, a toggle's or a slider's text, at its left; a HUD badge's lines, split at `'\n'`.
  - `UIA_Value` -- a toggle's (0 or 1), a slider's, a selector's option or a list's row (-1 none), a split pane's left width (0: its left side's own width).
  - `UIA_Min`, `UIA_Max` -- a slider's range: 0 to 100.
  - `UIA_Items`, `UIA_Count` -- a selector's options, a list's rows.
  - `UIA_Buffer`, `UIA_Size` -- a text field's line, ended by 0, and its bytes, the end's among them.
  - `UIA_Variant` -- a toggle's `TOGGLE_COBALT` or `TOGGLE_AMBER`; a label's `ALIGN_LEFT`, `ALIGN_CENTER` or `ALIGN_RIGHT`.
  - `UIA_Glyph` -- an amber toggle's text on its track (`"|||"`, `"%"`, `"L/R"`, `"2"`).
  - `UIA_Disabled` -- drawn muted, and deaf to events.
  - `UIA_Weight` -- a share of the room the gadget's group has left over: 0, none.
  - `UIA_Align` -- a group's children across it, or one child's own: `UIALIGN_FILL` (a column's default), `UIALIGN_START`, `UIALIGN_CENTER` (a row's), `UIALIGN_END`.
  - `UIA_Spacing`, `UIA_Margin` -- a group's between its children (8) and around them (0).
  - `UIA_Width`, `UIA_Height` -- the least size, over the gadget's own.
  - `UIA_Pane` -- what a group's drawn on: `UIPANE_DARK` or `UIPANE_LIGHT`; `UIPANE_NONE`, its group's (the root's, light); `UIPANE_BACKFILL` (V8 step 7), the caller's own: wherever ui.library would fill that pane it calls `ui_Backfill` with the rectangle, clipped as it draws, and its text is as on a dark pane. With no `ui_Backfill`, nothing's drawn there.

**RESULT**
The gadget's number, or 0 if the UI has `UI_GADGETS` already, `kind` isn't one, `parent` isn't a group that's been added, a split pane has its two, or the root's added twice.

**EXAMPLE**
From `conformance/uiwin.h`:
```c
AddGadgetTags(IUI, ui, 0, UI_SPLITPANE, { UIA_Value, 240 });
AddGadgetTags(IUI, ui, UW_SPLIT, UI_COLUMN, { UIA_Margin, 14 }, { UIA_Spacing, 10 });
AddGadgetTags(IUI, ui, UW_LEFTCOL, UI_TOGGLE, { UIA_Label, (uint64_t)"Mixer" },
              { UIA_Variant, TOGGLE_AMBER }, { UIA_Glyph, (uint64_t)"L/R" });
```

**NOTES**
- **Planar's own.** GadTools' `CreateGadget` made a gadget from a `NewGadget` and a kind; here the tree's the caller's, and layout is the library's.

**BUGS**
None known.

---

## ui/DrawUI

**NAME**
DrawUI -- every gadget drawn

**SYNOPSIS**
```c
#include <planar/ui.h>
void DrawUI(struct UI *ui);
```

**FUNCTION**
Draws the whole inside in `ui_RPort`: each pane's gradient, then every gadget on it. The damage becomes the whole inside: one rectangle in `ui_Damage`.

**INPUTS**
- `ui` -- laid out by [LayoutUI](#uilayoutui), its `ui_RPort` set.

**RESULT**
None. Without a rastport or a size, nothing.

**NOTES**
- A gadget drawn again by an event gives the pixels it has when the whole inside is drawn: `make ui-test` holds the two to each other after every step of its script.

**BUGS**
None known.

---

## ui/FreeUI

**NAME**
FreeUI -- the UI's fonts closed

**SYNOPSIS**
```c
#include <planar/ui.h>
void FreeUI(struct UI *ui);
```

**FUNCTION**
Closes the fonts [InitUI](#uiinitui) opened. The UI's memory is the caller's to free.

**BUGS**
None known.

---

## ui/HandleUIEvent

**NAME**
HandleUIEvent -- an IDCMP event given to the gadgets

**SYNOPSIS**
```c
#include <planar/ui.h>
uint32_t HandleUIEvent(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *change);
```

**FUNCTION**
Gives the gadgets an event from the window's IDCMP ([GetIntuiEvent](intuition.md#intuitiongetintuievent)). It reads `IDCMP_MOUSEBUTTONS`, `IDCMP_MOUSEMOVE`, `IDCMP_VANILLAKEY` and `IDCMP_RAWKEY`, and ignores the rest. Whatever it changes is drawn again in `ui_RPort`, and its rectangle added to `ui_Damage`.

- **The mouse.** A press focuses the gadget under it, if it takes keys. A toggle changes when the button's let go of over it, and a button is clicked the same way, drawn pressed while the pointer's on it. A slider's knob is dragged, or jumps to where its track's clicked. A selector's option and a list's row are chosen on the press; a second press on the same row within `UI_DOUBLE_NS` (500 ms) is a double click. A text field's cursor goes to the nearest character. A split pane's groove is dragged, both sides kept at least their own widths.
- **Keys**, to the focused gadget. Tab moves the focus to the next gadget that takes it, and Shift-Tab back. A text field takes characters (UTF-8), Backspace, Delete, Ctrl-U (the whole line), the arrows, Home and End, and Return. A toggle or a button takes Space or Return. A slider takes the arrows (Shift: a tenth of its range), Home and End; a selector, Left and Right; a list, Up, Down, Home, End, and Return as a double click.

**INPUTS**
- `ui` -- laid out.
- `ev` -- the event, in the window's inside's coordinates.
- `change` -- set to the change: `uc_Gadget`, `uc_Value` (the new value; a text field's, its length; a button's, 1) and `uc_Flags`, `UICF_DOUBLE` (a list's double click or Return), `UICF_ENTER` (Return in a text field) or `UICF_DRAG` (a slider or the split while the button's still down: more will follow). Zeroed if nothing changed.

**RESULT**
The gadget whose value changed, or 0. A text field's editing isn't a change: its Return is.

**EXAMPLE**
From `conformance/window.c`:
```c
if (IUI->HandleUIEvent(ui, &ev, &c))
    *last = c;
if (ui->ui_NumDamage && IIntuition->PresentWindow(w, ui->ui_Damage, ui->ui_NumDamage) != 0)
    return false;
ui->ui_NumDamage = 0;
```

**NOTES**
- With one buffer, wait for the last frame to be safe ([WaitSafe](intuition.md#intuitionwaitsafe)) before handing over an event, since it may draw.
- `IDCMP_NEWSIZE` isn't read: on it, call [LayoutUI](#uilayoutui) and [DrawUI](#uidrawui) with the new size, after giving the window its new buffers.

**BUGS**
None known.

---

## ui/InitUI

**NAME**
InitUI -- a UI made ready

**SYNOPSIS**
```c
#include <planar/ui.h>
int64_t InitUI(struct UI *ui, const struct GfxIFace *gfx);
```

**FUNCTION**
Zeroes the UI, keeps `gfx`, through which every gadget's drawn, and opens System Sans 13 px, regular and bold, and System Mono 13 px with it.

**INPUTS**
- `ui` -- the caller's: `sizeof(struct UI)`, about 6 KiB.
- `gfx` -- the caller's `IGfx`.

**RESULT**
0, or `ERR_NOTFOUND` if a font wouldn't open; nothing's left open then.

**BUGS**
None known.

---

## ui/LayoutUI

**NAME**
LayoutUI -- every gadget measured and placed

**SYNOPSIS**
```c
#include <planar/ui.h>
void LayoutUI(struct UI *ui, uint32_t width, uint32_t height);
```

**FUNCTION**
Measures every gadget, its children first, and places the root over an inside of `width` by `height`, each group's children along it. A row's go left to right and a column's top to bottom, `UIA_Spacing` apart and `UIA_Margin` in from its edges. Each is its own size along the group, plus its share by `UIA_Weight` of the room left over. Across the group, each is as `UIA_Align` says. A split pane's left side is `UIA_Value` wide and its right side the rest, past a 2 px groove. A window too small for it moves the split only while it is: its value's kept.

A gadget's own size, which a group adds up, comes from §13.3's sizes and its text in System Sans: a toggle 52 by 22, a slider's track 140, a selector's options the widest one's each, a button 80 at least, a list four rows of 22, a text field 160 by 24. A gadget that takes keys has 2 px around it for its focus ring.

**INPUTS**
- `ui` -- with its gadgets added.
- `width`, `height` -- the window's inside.

**RESULT**
None. Where there isn't room, gadgets keep their own sizes and the last ones are cut off.

**BUGS**
None known.

---

## ui/SetGadgetAttrsA

**NAME**
SetGadgetAttrsA -- a gadget's attributes changed

**SYNOPSIS**
```c
#include <planar/ui.h>
void SetGadgetAttrsA(struct UI *ui, uint32_t gadget, const struct TagItem *tags);
#define SetGadgetAttrs(iface, ui, gadget, ...)
```

**FUNCTION**
Sets the attributes `tags` gives, as [AddGadget](#uiaddgadget) takes them, holds the value to its range, and draws the gadget again if the UI's been laid out, its rectangle damaged. A split pane's children are placed again. A change to what a gadget's measured by, a label or the items, isn't laid out again until [LayoutUI](#uilayoutui).

**INPUTS**
- `ui` -- the UI.
- `gadget` -- its number; one that isn't is ignored.
- `tags` -- `UIA_*`.

**RESULT**
None. No change is reported: the caller made it.

**NOTES**
- The RKM's `GT_SetGadgetAttrs` by its idea, for a UI's gadgets.

**BUGS**
None known.
