# gfx: bitmaps, rastports and drawing

*Checked against V7 step 2; the paths, V8 step 1; masks, fills and type, V8 step 3.*

gfx.library's functions, in alphabetical order. The header is `planar/gfx.h`. They're a shared library's, called through `IGfx` (`struct GfxIFace`, the "main" interface, version 1, of gfx.library 8; a program that opens version 7 gets the same table, and V8's calls are at its end). The same functions by the same names are in `lib/gfx` (`libgfx.a`), scalar, for a program that links them. None traps, except for the Area `AllocBitMap` makes and `FreeBitMap` frees. None keeps state but an outline font's glyph cache, which is in the font's own Area: any task can call any of them at any time, but a `TextFont`, like a `RastPort`, is one task's at a time. [Libraries ch. 12](../libraries/12-gfx.md) is the guide to them.

---

## gfx/AllocBitMap

**NAME**
AllocBitMap -- make a bitmap in an Area of the caller's

**SYNOPSIS**
```c
#include <planar/gfx.h>
struct BitMap *AllocBitMap(uint32_t width, uint32_t height, uint32_t format, uint32_t flags,
                           const struct BitMap *friend_bm);
```

**FUNCTION**
Allocates one Area of the calling process's, zeroed, with `height` rows of `width` pixels from its start and the `BitMap` that describes them after the last row. Rows are a multiple of 64 bytes apart (`GFX_ROW_ALIGN`).

With `BMF_DISPLAYABLE` and a friend:
- the Area is placed as the friend's is: `AREAF_NC` or `AREAF_DMA32` if `QueryArea` says the friend's memory has them;
- a bitmap as wide as its friend takes the friend's `bm_BytesPerRow`, if it's no less than its own.

**INPUTS**
- `width`, `height` -- in pixels, 1 to `GFX_MAX_SIZE` (16384).
- `format` -- `PIXFMT_XRGB8888` or `PIXFMT_ARGB8888`.
- `flags` -- `BMF_CLEAR` (it always is: an Area is zeroed), `BMF_DISPLAYABLE`. Kept in `bm_Flags`, with `BMF_CLEAR` always set.
- `friend_bm` -- a bitmap to be placed as, or `nullptr`. Read only with `BMF_DISPLAYABLE`.

**RESULT**
The bitmap, or `nullptr`:
- if a size is 0 or too large;
- if the format or a flag isn't known;
- if `AllocArea` fails.

**EXAMPLE**
```c
struct BitMap *bm = IGfx->AllocBitMap(64, 48, PIXFMT_ARGB8888, 0, nullptr);
```

**NOTES**
- **Differs from Exec:** V39's took a depth and allocated bitplanes in Chip RAM. This takes a pixel format.
- `bm_Area` is the Area's handle, the process's: `ShareArea` it to let another process read the bitmap. The `BitMap` goes with it.
- The friend's Area isn't held or shared by this: only its placement is copied (`lib/gfx/bitmap.c`).

**BUGS**
None known.

**SEE ALSO**
[FreeBitMap](#gfxfreebitmap), [InitBitMap](#gfxinitbitmap), [AllocArea](exec.md#execallocarea), [ShareArea](exec.md#execsharearea).

---

## gfx/BltBitMap

**NAME**
BltBitMap -- copy or blend a rectangle into a rastport

**SYNOPSIS**
```c
#include <planar/gfx.h>
void BltBitMap(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
               int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend);
```

**FUNCTION**
Takes the `xSize` by `ySize` rectangle at (`xSrc`, `ySrc`) of `src` to (`xDest`, `yDest`) in `dst`. The rectangle is clipped to the source bitmap, then to the rastport's clip, both corners moving together.
- `BLEND_SRC`: the source's pixels, as they are.
- `BLEND_SRC_OVER`: an ARGB source over the destination, which is taken as opaque. Each colour channel becomes `(s × a + d × (255 − a)) / 255`, and alpha `a + da × (255 − a) / 255`, each rounded to the nearest. An XRGB source is opaque: it's copied.

Source and destination may be the same bitmap and overlap in any direction. The result is as though the whole source were read first.

**INPUTS**
- `src` -- the bitmap to read.
- `xSrc`, `ySrc` -- the rectangle's top left in it. May be negative.
- `dst` -- the rastport to draw in.
- `xDest`, `yDest` -- where the top left goes. May be negative.
- `xSize`, `ySize` -- the rectangle's size. 0 or less draws nothing.
- `blend` -- `BLEND_SRC` or `BLEND_SRC_OVER`.

**RESULT**
None. Nothing is drawn for an unknown `blend`, a `nullptr` bitmap or rastport, or a rectangle clipped away.

**NOTES**
- **Differs from Exec:** graphics.library's took two bitmaps, a minterm and a plane mask. This draws into a rastport, by Porter-Duff blends ([docs §16.4](../../docs/16-rtg-display-gpu.md)).
- By the vector unit where the CPU has one (`arch/<isa>/lib/gfx.c`), to the same bit as the scalar entry.
- Two different bitmaps whose memory overlaps aren't supported: the order of rows is chosen as for one bitmap (`lib/gfx/draw.c`).

**BUGS**
None known.

**SEE ALSO**
[RectFill](#gfxrectfill), [InitRastPort](#gfxinitrastport).

---

## gfx/BltMask

**NAME**
BltMask -- blend a coverage mask in the foreground pen, in linear light

**SYNOPSIS**
```c
#include <planar/gfx.h>
void BltMask(struct RastPort *rp, const struct GfxMask *mask, int32_t x, int32_t y);
```

**FUNCTION**
Blends `rp_FgPen` into the rastport through the mask's coverage, a byte a pixel. Mask pixel (i, j) lands on (`x + gm_X + i`, `y + gm_Y + j`), so a mask from `PathMask` lands where its path is with `x` and `y` 0. Each colour channel is mixed in linear light ([docs §14.3.4](../../docs/14-font-engine.md)): the pen's and the pixel's taken to 16 bits of linear light by a 256-entry table, mixed by the coverage, and taken back to sRGB by a 4,096-entry table indexed by the top 12 bits. Alpha is mixed as it is. Coverage 0 leaves a pixel alone, and 255 writes the pen, top byte included. Clipped to the rastport.

**INPUTS**
- `rp` -- the rastport, and its foreground pen.
- `mask` -- `gm_Pixels`, `gm_Width`, `gm_Height`, `gm_BytesPerRow` (at least the width), and `gm_X`, `gm_Y`.
- `x`, `y` -- added to `gm_X` and `gm_Y`.

**RESULT**
None. Nothing is drawn for a `nullptr` mask or rastport, or rows shorter than the mask is wide.

**EXAMPLE**
```c
struct GfxMask m = { pixels, w, h, w, r.MinX, r.MinY };

IGfx->PathMask(&path, 0, &m, scratch, IGfx->PathMaskScratch(w, h));
rp.rp_FgPen = 0xffe8eef6;
IGfx->BltMask(&rp, &m, 0, 0);
```

**NOTES**
- **Planar's own:** graphics.library's `BltTemplate` drew a 1-bit template; this blends 8-bit coverage.
- The coverage is a weight out of 256: `c + c / 128`, so 255 is all of it. Within 1/255 of the exact blend in linear light for every colour, under every colour, at every coverage (`make gfx-test`); 8% are 1/255 off.
- Light text on a dark ground and dark text on a light one keep the same weight, which blending in sRGB doesn't give.
- By the vector unit where the CPU has one: RVV looks the tables up by indexed loads; NEON, which has no gather, takes runs of 16 clear or covered pixels at once and the rest one at a time.

**BUGS**
None known.

**SEE ALSO**
[PathMask](#gfxpathmask), [FillPath](#gfxfillpath), [TextRun](#gfxtextrun).

---

## gfx/DrawBevel

**NAME**
DrawBevel -- a 1-pixel bevel, lit from the top left

**SYNOPSIS**
```c
#include <planar/gfx.h>
void DrawBevel(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
               uint32_t light, uint32_t dark);
```

**FUNCTION**
Draws the edges of the rectangle (`xMin`, `yMin`) to (`xMax`, `yMax`), inclusive, as [docs §13.2](../../docs/13-workbench.md) lights them. The top and left edges are `light`, and the bottom and right edges `dark`, drawn after, so the dark ones take the top right and bottom left corners. Clipped to the rastport.

**INPUTS**
- `rp` -- the rastport.
- `xMin`, `yMin`, `xMax`, `yMax` -- the rectangle. Nothing is drawn if `xMin > xMax` or `yMin > yMax`.
- `light`, `dark` -- the two pens, as pixels.

**RESULT**
None.

**NOTES**
- Planar's: graphics.library had none. gadtools' `DrawBevelBox` drew a 2-pixel one from a tag list.
- By the vector unit where the CPU has one.

**BUGS**
None known.

**SEE ALSO**
[RectFill](#gfxrectfill).

---

## gfx/FillGradient

**NAME**
FillGradient -- fill a rectangle or a path with a two-stop gradient

**SYNOPSIS**
```c
#include <planar/gfx.h>
void FillGradient(struct RastPort *rp, const struct GfxGradient *g, const struct GfxPath *path,
                  uint32_t flags);
```

**FUNCTION**
Fills with colours that go from `gg_From` at one edge of `gg_Rect` to `gg_To` at the other: top to bottom, or left to right with `GRADF_HORIZONTAL`. Each channel, alpha among them, follows a straight line in sRGB, rounded to the nearest; past an edge it's that edge's colour. With no path, the rectangle is filled, the colours written as they are, as `RectFill` writes its pen. With a path, the path is filled as `FillPath` fills it, each pixel's colour the gradient's there.

**INPUTS**
- `rp` -- the rastport.
- `g` -- the rectangle (inclusive), the two colours, and `GRADF_HORIZONTAL` or 0.
- `path` -- `nullptr` for the rectangle, or the path to fill.
- `flags` -- the path's fill rule, `PATHF_EVENODD` or 0.

**RESULT**
None.

**EXAMPLE**
From `conformance/typescene.h`:
```c
struct GfxGradient bar = { { 8, 0, 151, 0 }, 0xffd8dee8, 0xff8bb4e8, GRADF_HORIZONTAL };

IGfx->PathRoundRect(&path, &(struct GfxRect){ 8, 6, 151, 30 }, 6 * 64);
IGfx->FillGradient(&rp, &bar, &path, 0);
```

**NOTES**
- **Planar's own:** for §13.2's bars, backdrop and gadgets ([docs §13.2](../../docs/13-workbench.md)).
- A horizontal gradient's `MinY` and `MaxY` aren't read, nor a vertical one's `MinX` and `MaxX`.
- No dithering: a gradient over many pixels between near colours shows bands.

**BUGS**
None known.

**SEE ALSO**
[FillPath](#gfxfillpath), [PathRoundRect](#gfxpathroundrect).

---

## gfx/FillPath

**NAME**
FillPath -- fill a path in the foreground pen, anti-aliased

**SYNOPSIS**
```c
#include <planar/gfx.h>
void FillPath(struct RastPort *rp, const struct GfxPath *path, uint32_t flags);
```

**FUNCTION**
Fills the path with `rp_FgPen`: its coverage, as `PathMask` works it out, blended as `BltMask` blends it. Only the path's box within the clip is worked on, a piece at a time: as many rows of it, as wide as fit, as the scratch holds. The scratch is the rastport's `TmpRas` if it has one, or a little of the stack. Each piece is the same bytes as a mask of the whole would be, so the pieces don't show.

**INPUTS**
- `rp` -- the rastport, its pen, and `rp_TmpRas` (or `nullptr`).
- `path` -- 26.6 fixed point, y down.
- `flags` -- `PATHF_EVENODD`, or 0 for non-zero winding.

**RESULT**
None. Nothing is drawn for a path `PathBounds` refuses.

**EXAMPLE**
A ring, from `conformance/typescene.h`:
```c
IGfx->PathRoundRect(&path, &(struct GfxRect){ 110, 52, 150, 92 }, 20 * 64);
IGfx->PathRoundRect(&path, &(struct GfxRect){ 120, 62, 140, 82 }, 10 * 64);
rp.rp_FgPen = 0xffe0a040;
IGfx->FillPath(&rp, &path, PATHF_EVENODD);
```

**NOTES**
- **Planar's own:** the nearest of graphics.library's is `AreaEnd`, which needed a `TmpRas` too, and filled without anti-aliasing.
- Each piece walks the whole path again. Without a `TmpRas`, a piece is 2.5 KB of the stack (64 pixels by 7 rows), so a big path costs more than with one; with a `TmpRas` of a few tens of KB, a screen-wide path is a handful of pieces.
- By the vector unit where the CPU has one: the sweep and the blend.

**BUGS**
None known.

**SEE ALSO**
[PathMask](#gfxpathmask), [BltMask](#gfxbltmask), [FillGradient](#gfxfillgradient), [InitTmpRas](#gfxinittmpras), [PathRoundRect](#gfxpathroundrect).

---

## gfx/FreeBitMap

**NAME**
FreeBitMap -- free a bitmap AllocBitMap made

**SYNOPSIS**
```c
#include <planar/gfx.h>
void FreeBitMap(struct BitMap *bm);
```

**FUNCTION**
Frees the Area `bm->bm_Area` names, and so the pixels and the `BitMap` itself. Does nothing for `nullptr`, or for a bitmap `InitBitMap` described (`bm_Area` 0).

**INPUTS**
- `bm` -- the bitmap.

**RESULT**
None.

**NOTES**
- A process the Area was shared with keeps its mapping until it lets go, as with any Area ([FreeArea](exec.md#execfreearea)).

**BUGS**
None known.

**SEE ALSO**
[AllocBitMap](#gfxallocbitmap).

---

## gfx/GetBitMapAttr

**NAME**
GetBitMapAttr -- what a bitmap says of itself

**SYNOPSIS**
```c
#include <planar/gfx.h>
uint64_t GetBitMapAttr(const struct BitMap *bm, uint32_t attr);
```

**FUNCTION**
Returns one of the bitmap's attributes:

| `attr` | Value |
|---|---|
| `BMA_WIDTH`, `BMA_HEIGHT` | In pixels |
| `BMA_DEPTH` | Bits a pixel: 32 |
| `BMA_FLAGS` | `bm_Flags` |
| `BMA_PIXELFORMAT` | `PIXFMT_*` |
| `BMA_BYTESPERROW` | Bytes from one row to the next |
| `BMA_MODIFIER` | `MOD_LINEAR` |
| `BMA_BASEADDR` | The top left pixel's address |
| `BMA_COLORSPACE` | `CS_SRGB` |
| `BMA_AREA` | The Area's handle, or 0 |

**INPUTS**
- `bm` -- the bitmap.
- `attr` -- a `BMA_*`.

**RESULT**
The attribute; 0 for an unknown one or a `nullptr` bitmap.

**NOTES**
- V39's queries, with its numbers for the ones it had (`BMA_HEIGHT` 0, `BMA_DEPTH` 4, `BMA_WIDTH` 8, `BMA_FLAGS` 12). The rest are Planar's ([docs §16.3](../../docs/16-rtg-display-gpu.md)).

**BUGS**
None known.

**SEE ALSO**
[AllocBitMap](#gfxallocbitmap).

---

## gfx/InitBitMap

**NAME**
InitBitMap -- describe memory the caller has as a bitmap

**SYNOPSIS**
```c
#include <planar/gfx.h>
bool InitBitMap(struct BitMap *bm, void *base, uint32_t width, uint32_t height,
                uint32_t bytesPerRow, uint32_t format);
```

**FUNCTION**
Fills in `*bm` for `height` rows of `width` pixels at `base`, `bytesPerRow` apart. Its `bm_Area` and `bm_Flags` are 0. Nothing is allocated.

**INPUTS**
- `bm` -- the `BitMap` to fill in.
- `base` -- the top left pixel: 4-byte aligned.
- `width`, `height` -- 1 to `GFX_MAX_SIZE`.
- `bytesPerRow` -- a multiple of 4, at least `width × 4`.
- `format` -- `PIXFMT_XRGB8888` or `PIXFMT_ARGB8888`.

**RESULT**
True; false, with `*bm` left alone, if an argument is out of range.

**EXAMPLE**
A display buffer (`conformance/displaytest.c`):
```c
IGfx->InitBitMap(&screen, (void *)a->px, a->width, a->height, a->stride, PIXFMT_XRGB8888);
```

**NOTES**
- **Differs from Exec:** V36's set up the planes' layout, and the caller filled in `Planes[]`. This takes the one address.

**BUGS**
None known.

**SEE ALSO**
[AllocBitMap](#gfxallocbitmap), [InitRastPort](#gfxinitrastport).

---

## gfx/InitRastPort

**NAME**
InitRastPort -- a rastport over a whole bitmap

**SYNOPSIS**
```c
#include <planar/gfx.h>
void InitRastPort(struct RastPort *rp, struct BitMap *bm);
```

**FUNCTION**
Sets `*rp` up to draw anywhere in `bm`:
- the clip is the whole bitmap;
- the foreground pen is white (`0xffffffff`), the background black (`0xff000000`);
- the mode is `JAM2`;
- the font is the 8×8 (`nullptr`);
- there's no `TmpRas` (`nullptr`).

**INPUTS**
- `rp` -- the rastport to set up. `nullptr` does nothing.
- `bm` -- its bitmap. With `nullptr` the clip is empty.

**RESULT**
None.

**NOTES**
- **Differs from Exec:** the RKM's had a `Layer`, an area fill's workspace (`AreaInfo`) and a pen position. This has none of them, but a `TmpRas` (V8): change its fields directly, or with `SetFont` and `InitTmpRas`.

**BUGS**
None known.

**SEE ALSO**
[RectFill](#gfxrectfill), [Text](#gfxtext), [SetFont](#gfxsetfont), [InitTmpRas](#gfxinittmpras).

---

## gfx/InitTmpRas

**NAME**
InitTmpRas -- give a TmpRas its memory

**SYNOPSIS**
```c
#include <planar/gfx.h>
void InitTmpRas(struct TmpRas *tr, void *buffer, uint64_t size);
```

**FUNCTION**
Sets `tr_RasPtr` to `buffer` and `tr_Size` to `size` (0 if `buffer` is `nullptr`). A rastport whose `rp_TmpRas` points at it fills paths in pieces as big as the buffer holds, instead of in 2.5 KB of the stack.

**INPUTS**
- `tr` -- the `TmpRas`, the caller's.
- `buffer`, `size` -- the scratch: any alignment. Under 2.5 KB is no better than the stack, and isn't used.

**RESULT**
None.

**EXAMPLE**
```c
static uint8_t scratch[32768];
struct TmpRas tr;

IGfx->InitTmpRas(&tr, scratch, sizeof scratch);
rp.rp_TmpRas = &tr;
```

**NOTES**
- **As the RKM's,** whose `AreaEnd` and `Flood` needed one. A piece of a path `w` pixels wide takes `(w + 2) × 4 + w` bytes a row.
- The buffer is the caller's, and one task's at a time, as the rastport is.

**BUGS**
None known.

**SEE ALSO**
[FillPath](#gfxfillpath), [InitRastPort](#gfxinitrastport).

---

## gfx/OpenTextFont

**NAME**
OpenTextFont, CloseTextFont -- one of font.library's fonts at a size, and its glyph cache

**SYNOPSIS**
```c
#include <planar/gfx.h>
struct TextFont *OpenTextFont(const struct TextAttr *ta);
void CloseTextFont(struct TextFont *tf);
```

**FUNCTION**
Opens a system font at a size for drawing: `ta_Name` `"System Sans"` (or `nullptr`) or `"System Mono"`, `ta_YSize` pixels an em, and `FSF_BOLD` in `ta_Style` for the bold. The `TextFont` and everything it needs are in one new Area of the caller's: the face, a glyph cache, an outline's worth of points, and the rasteriser's scratch. gfx.library finds font.library itself, through the program's `IExec`.

A glyph is drawn into the cache the first time it's needed, at the subpixel phase it's needed at, and blended from there after. System Sans at 12 to 16 px is placed to a quarter pixel, four phases a glyph ([VQ35](../../docs/14-font-engine.md)); other sizes, and System Mono, to whole pixels. When the cache is full it's emptied and fills again.

`CloseTextFont` frees the Area, and the `TextFont` with it.

**INPUTS**
- `ta` -- the font's name, size (1 to 255) and style; `ta_Flags` 0.
- `tf` -- what `OpenTextFont` gave, or `nullptr`.

**RESULT**
The font: `tf_YSize` the line's height in pixels (`LineSpacing`'s, rounded), `tf_Baseline` the line's top to its baseline, `tf_Flags` `FPF_OUTLINE` (and `FPF_FIXED` for System Mono, whose `tf_XSize` is its advance, rounded), `tf_Size` the size in 26.6. `nullptr` for a name it doesn't know, a size out of range, no memory, or no font.library.

**EXAMPLE**
From `conformance/typetest.c`:
```c
struct TextFont *tf = IGfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 });

IGfx->SetFont(&rp, tf);
IGfx->Text(&rp, 8, 36, "Type, at last", 13);
IGfx->SetFont(&rp, nullptr);
IGfx->CloseTextFont(tf);
```

**NOTES**
- **Differs from Exec:** graphics.library's `OpenFont` found a bitmap font in memory, and diskfont.library's `OpenDiskFont` loaded one. This opens an outline font at any size, and is named apart from font.library's `OpenFont`, which opens a face.
- **The cache is the process's own** (v8-plan decision 4), in its Area: nothing is shared, and a process that dies takes it with it. Its arena is 32 glyph boxes' worth, from 64 KB to 4 MB; 1,024 slots, emptied at 768.
- One task at a time: two tasks drawing in one font at once must each open their own.
- Only the system fonts in V8; fonts from files come later.

**BUGS**
None known.

**SEE ALSO**
[SetFont](#gfxsetfont), [Text](#gfxtext), [TextRun](#gfxtextrun), [TextLength](#gfxtextlength), [ShapeRun](font.md#fontshaperun).

---

## gfx/PathBounds

**NAME**
PathBounds -- the pixels a path's points touch

**SYNOPSIS**
```c
#include <planar/gfx.h>
bool PathBounds(const struct GfxPath *path, struct GfxRect *r);
```

**FUNCTION**
Finds the box of every point in the path, curves' control points among them, and rounds it out to whole pixels: the smallest rectangle of pixels the path can cover. It's the box to give `PathMask`.

**INPUTS**
- `path` -- 26.6 fixed point, y down (`struct GfxPath`, below).
- `r` -- where the box goes, inclusive, in pixels.

**RESULT**
True, with `*r` set. False, with `*r` untouched, if the path has no points, spans no area, starts with something other than `PATH_MOVE`, or its verbs and points don't agree.

**EXAMPLE**
```c
struct GfxRect r;

if (IGfx->PathBounds(&path, &r))
    w = r.MaxX - r.MinX + 1, h = r.MaxY - r.MinY + 1;
```

**NOTES**
- **Planar's own:** Exec had no paths.
- A path's arrays are the caller's: `gp_Points` and `gp_Verbs`, `gp_MaxPoints` and `gp_MaxVerbs` their sizes, `gp_NumPoints` and `gp_NumVerbs` what's in them. Each verb takes its points from the next in `gp_Points`: `PATH_MOVE` and `PATH_LINE` one, `PATH_QUAD` two (the control, then the end), `PATH_CUBIC` three, `PATH_CLOSE` none.
- The box is FreeType's for the same outline: its bitmap's left, top, width and rows (`tools/font-oracle`, pass 4).

**BUGS**
None known.

**SEE ALSO**
[PathMask](#gfxpathmask), [GlyphOutline](font.md#fontglyphoutline).

---

## gfx/PathMask

**NAME**
PathMask, PathMaskScratch -- a path's coverage of each pixel

**SYNOPSIS**
```c
#include <planar/gfx.h>
uint64_t PathMaskScratch(uint32_t width, uint32_t height);
bool PathMask(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
              uint64_t scratchSize);
```

**FUNCTION**
Fills `mask` with how much of each of its pixels the path covers, a byte a pixel: 0 none, 255 all. Curves are cut into lines within 1/512 px of them, and each line's area is added up exactly, so every pixel is within 2/255 of the true coverage. A subpath is closed by a line back to its start whether or not it ends in `PATH_CLOSE`. Integer arithmetic throughout: the same path gives the same bytes on every CPU.

`PathMaskScratch` says how many bytes of scratch a mask of that size needs: `(width + 2) * height` 32-bit words.

**INPUTS**
- `path` -- 26.6 fixed point, y down.
- `flags` -- `PATHF_EVENODD` for the even-odd rule; otherwise non-zero winding, as TrueType's outlines are.
- `mask` -- `gm_Pixels`, `gm_Width` and `gm_Height` (each at most `GFX_MAX_SIZE`), `gm_BytesPerRow` (at least the width), and `gm_X`, `gm_Y`: which of the path's pixels the mask's top left is. `PathBounds` gives a box that holds it all.
- `scratch`, `scratchSize` -- at least `PathMaskScratch(width, height)` bytes, 4-aligned. Overwritten.

**RESULT**
True. False, with nothing written, if the path is malformed (as [PathBounds](#gfxpathbounds) says), the mask's sizes are wrong, or the scratch is short.

**EXAMPLE**
```c
struct GfxMask m = { pixels, w, h, w, r.MinX, r.MinY };

IGfx->PathMask(&path, 0, &m, scratch, IGfx->PathMaskScratch(w, h));
```

**NOTES**
- **Planar's own:** Exec had no paths.
- Part of a path outside the mask counts as it would: an edge left of the mask still covers the pixels right of it. A mask cut from a bigger one has the bigger one's bytes there, which is how `FillPath` draws in pieces. Nothing outside the mask is written.
- Each glyph of `font.library` is drawn so (v8-plan decision 6): [GlyphOutline](font.md#fontglyphoutline), then this.
- Held to 2/255 of the exact coverage over ten fonts' every glyph at five sizes by `tools/font-oracle`'s pass 4. FreeType's unhinted coverage differs from it by up to 29/255 on curves, where FreeType approximates them more coarsely.
- The row sweep (the running sum, folded or clamped, and scaled to a byte) is by the vector unit where the CPU has one (V8 step 3), in 32-bit sums that wrap: the same bytes as the 64-bit ones step 1 had for any path wound fewer than 16,384 times.

**BUGS**
None known.

**SEE ALSO**
[PathBounds](#gfxpathbounds), [GlyphOutline](font.md#fontglyphoutline).

---

## gfx/PathRoundRect

**NAME**
PathRoundRect -- add a rounded rectangle to a path

**SYNOPSIS**
```c
#include <planar/gfx.h>
bool PathRoundRect(struct GfxPath *path, const struct GfxRect *r, int32_t radius);
```

**FUNCTION**
Adds a subpath around the rectangle's pixels, from the left of `MinX` to the right of `MaxX` and the top of `MinY` to the bottom of `MaxY`, clockwise, its corners quarter circles of `radius` drawn as cubics. A radius of 0 gives square corners: 5 verbs and 4 points, which `FillPath` fills as `RectFill` would. Otherwise 10 verbs and 17 points. The path's existing subpaths stay, so two make a ring under `PATHF_EVENODD`.

**INPUTS**
- `path` -- the path to add to.
- `r` -- the rectangle, inclusive, in pixels.
- `radius` -- 26.6 px, cut to half the shorter side; negative is 0.

**RESULT**
True. False, with the path as it was, if its arrays haven't room, or `path` or `r` is `nullptr`. An empty rectangle adds nothing, and is true.

**EXAMPLE**
```c
IGfx->PathRoundRect(&path, &(struct GfxRect){ 8, 6, 151, 30 }, 6 * 64);
```

**NOTES**
- **Planar's own.**
- The cubics' controls are 0.5523 of the radius from the corners, so each corner is within 0.03% of a circle.

**BUGS**
None known.

**SEE ALSO**
[FillPath](#gfxfillpath), [FillGradient](#gfxfillgradient).

---

## gfx/RectFill

**NAME**
RectFill -- fill a rectangle with the foreground pen

**SYNOPSIS**
```c
#include <planar/gfx.h>
void RectFill(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax);
```

**FUNCTION**
Writes `rp_FgPen` to every pixel of the rectangle (`xMin`, `yMin`) to (`xMax`, `yMax`), inclusive, that's inside the rastport's clip. The pen is written as it is, top byte included: nothing is blended.

**INPUTS**
- `rp` -- the rastport.
- `xMin`, `yMin`, `xMax`, `yMax` -- the rectangle. May lie partly or wholly off the bitmap. Nothing is drawn if `xMin > xMax` or `yMin > yMax`.

**RESULT**
None.

**EXAMPLE**
```c
rp.rp_FgPen = 0x80c08040;
IGfx->RectFill(&rp, 0, 0, 63, 47);
```

**NOTES**
- **Differs from Exec:** the pen is a pixel, not a palette index, and there's no area pattern or outline.
- By the vector unit where the CPU has one.

**BUGS**
None known.

**SEE ALSO**
[BltBitMap](#gfxbltbitmap), [DrawBevel](#gfxdrawbevel).

---

## gfx/SetFont

**NAME**
SetFont -- a rastport's font

**SYNOPSIS**
```c
#include <planar/gfx.h>
void SetFont(struct RastPort *rp, const struct TextFont *tf);
```

**FUNCTION**
Sets `rp_Font`: what `Text`, `TextLength` and `TextRun` draw and measure in. `nullptr` is the 8×8.

**INPUTS**
- `rp` -- the rastport.
- `tf` -- a font from `OpenTextFont`, a bitmap font of the caller's, or `nullptr`.

**RESULT**
None.

**NOTES**
- **As the RKM's.** Its `rp->TxHeight` and `TxBaseline` are the font's own `tf_YSize` and `tf_Baseline` here.
- The rastport doesn't own the font: close the font after the last text drawn in it, and set another first if the rastport lives on.

**BUGS**
None known.

**SEE ALSO**
[OpenTextFont](#gfxopentextfont), [Text](#gfxtext).

---

## gfx/Text

**NAME**
Text -- draw characters in the rastport's font

**SYNOPSIS**
```c
#include <planar/gfx.h>
void Text(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count);
```

**FUNCTION**
Draws `count` bytes of `string`, the line's top left at (`x`, `y`).

In a bitmap font (the 8×8, or one of the caller's) each byte is a glyph, `tf_XSize` pixels on from the last. A glyph's set bits take `rp_FgPen`. In `JAM2` its clear bits take `rp_BgPen`, and in `JAM1` they're left alone. A byte the font doesn't hold is drawn as its first glyph. Clipped to the rastport, a pixel at a time.

In an outline font (`OpenTextFont`'s) the bytes are UTF-8, shaped by font.library's `ShapeRun` (kerned; ligatures too in System Sans) and drawn as `TextRun` draws them, on the baseline `y + tf_Baseline`. In `JAM2` the line's box, `tf_YSize` high and as wide as the text, is filled with `rp_BgPen` first. Text is shaped in pieces of up to 256 bytes, cut between characters.

**INPUTS**
- `rp` -- the rastport. Its font, or the 8×8 if `rp_Font` is `nullptr`.
- `x`, `y` -- the line's top left.
- `string` -- the bytes. Needn't end with a NUL.
- `count` -- how many.

**RESULT**
None. Nothing is drawn for a `nullptr` string or rastport, or a bitmap font wider than 8 pixels.

**NOTES**
- **Differs from Exec:** graphics.library's drew at the pen position (`Move`), on the baseline. This draws at the line's top left, in either kind of font: `y + tf_Baseline` is the baseline. `TextRun` takes a baseline.
- A bitmap `TextFont` is one byte a row, bit 0 leftmost: `tf_YSize` bytes a glyph, from `tf_LoChar` to `tf_HiChar`.
- The 8×8 font is U+0020 to U+007E, the same table as display's console (`lib/gfx/font8x8.c`).
- A kern or ligature across two pieces of 256 bytes is lost. Shape a long text once with `ShapeRun`, and draw it with `TextRun`.
- A bitmap font is drawn scalar; an outline font's glyphs are rasterised and blended by the vector unit where the CPU has one.

**BUGS**
None known.

**SEE ALSO**
[InitRastPort](#gfxinitrastport), [SetFont](#gfxsetfont), [TextLength](#gfxtextlength), [TextRun](#gfxtextrun).

---

## gfx/TextLength

**NAME**
TextLength -- how far Text would go

**SYNOPSIS**
```c
#include <planar/gfx.h>
int32_t TextLength(struct RastPort *rp, const char *string, uint64_t count);
```

**FUNCTION**
The width `Text` would draw `count` bytes of `string` in, in pixels: in an outline font, the shaped run's advance at its size, rounded; in a bitmap font, `count` times `tf_XSize`.

**INPUTS**
- `rp` -- the rastport, for its font.
- `string`, `count` -- as `Text`'s.

**RESULT**
Pixels; 0 for a `nullptr` rastport or string.

**EXAMPLE**
Centring a title:
```c
int32_t w = IGfx->TextLength(&rp, title, n);

IGfx->Text(&rp, (width - w) / 2, 4, title, n);
```

**NOTES**
- **As the RKM's.**

**BUGS**
None known.

**SEE ALSO**
[Text](#gfxtext), [FitLine](font.md#fontfitline).

---

## gfx/TextRun

**NAME**
TextRun -- draw a shaped run's glyphs

**SYNOPSIS**
```c
#include <planar/gfx.h>
void TextRun(struct RastPort *rp, const struct GlyphRun *run, uint32_t first, uint32_t count,
             int32_t x, int32_t y);
```

**FUNCTION**
Draws glyphs `first` to `first + count - 1` of a run `ShapeRun` made in the rastport's font's face, in `rp_FgPen`, the pen starting at `x` on the baseline `y`. Each glyph lands at the pen plus its offset: the run's font units summed from `first`, then scaled to the size, and rounded to the font's nearest phase (a quarter pixel, or a pixel). Its coverage comes from the font's cache, and is blended as `BltMask` blends. Hidden glyphs (`GLYPHF_HIDDEN`: controls, a tab, a soft hyphen not taken) move the pen and draw nothing.

**INPUTS**
- `rp` -- the rastport: its font must be an outline font.
- `run` -- the shaped run, in the same face and style.
- `first`, `count` -- the glyphs to draw; cut to the run's end.
- `x` -- the pen's start, in pixels.
- `y` -- the baseline.

**RESULT**
None. Nothing is drawn if the font isn't an outline font, or `first` is past the run's end.

**EXAMPLE**
A paragraph, a line at a time:
```c
struct LineFit fit;

for (uint32_t i = 0, y = top + tf->tf_Baseline; i < run.gr_Count; i += fit.lf_Count, y += tf->tf_YSize) {
    IFont->FitLine(&face, &run, i, width << 6, tf->tf_Size, &fit);
    IGfx->TextRun(&rp, &run, i, fit.lf_Count, left, y);
}
```

**NOTES**
- **Planar's own** ([docs §16.4](../../docs/16-rtg-display-gpu.md)'s `GfxDrawGlyphRun`).
- A line drawn from `FitLine`'s `lf_Count` includes its trailing spaces and newline, which draw nothing. Draw `lf_Hyphen` after it when `LINEF_HYPHEN` says so.

**BUGS**
None known.

**SEE ALSO**
[Text](#gfxtext), [ShapeRun](font.md#fontshaperun), [FitLine](font.md#fontfitline), [OpenTextFont](#gfxopentextfont).
