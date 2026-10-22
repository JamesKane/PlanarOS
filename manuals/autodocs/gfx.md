# gfx: bitmaps, rastports and drawing

*Checked against V7 step 2.*

gfx.library's functions, in alphabetical order. The header is `planar/gfx.h`. They're a shared library's, called through `IGfx` (`struct GfxIFace`, the "main" interface, version 1, of gfx.library 7). The same functions by the same names are in `lib/gfx` (`libgfx.a`), scalar, for a program that links them. None traps, except for the Area `AllocBitMap` makes and `FreeBitMap` frees. None keeps state, so any task can call any of them at any time. [Libraries ch. 12](../libraries/12-gfx.md) is the guide to them.

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
- the font is the 8×8 (`nullptr`).

**INPUTS**
- `rp` -- the rastport to set up. `nullptr` does nothing.
- `bm` -- its bitmap. With `nullptr` the clip is empty.

**RESULT**
None.

**NOTES**
- **Differs from Exec:** the RKM's had a `Layer`, an area fill's workspace and a pen position. This has none of them: change its fields directly.

**BUGS**
None known.

**SEE ALSO**
[RectFill](#gfxrectfill), [Text](#gfxtext).

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

## gfx/Text

**NAME**
Text -- draw characters in the rastport's font

**SYNOPSIS**
```c
#include <planar/gfx.h>
void Text(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count);
```

**FUNCTION**
Draws `count` bytes of `string`, the first glyph's top left at (`x`, `y`), each next one `tf_XSize` pixels on. A glyph's set bits take `rp_FgPen`. In `JAM2` its clear bits take `rp_BgPen`, and in `JAM1` they're left alone. A byte the font doesn't hold is drawn as its first glyph. Clipped to the rastport, a pixel at a time.

**INPUTS**
- `rp` -- the rastport. Its font, or the 8×8 if `rp_Font` is `nullptr`.
- `x`, `y` -- the first glyph's top left.
- `string` -- the bytes. Needn't end with a NUL.
- `count` -- how many.

**RESULT**
None. Nothing is drawn for a `nullptr` string or rastport, or a font wider than 8 pixels.

**NOTES**
- **Differs from Exec:** graphics.library's drew at the pen position (`Move`), on the baseline. This draws at the top left given, until V8's fonts give a baseline.
- A `TextFont` is one byte a row, bit 0 leftmost: `tf_YSize` bytes a glyph, from `tf_LoChar` to `tf_HiChar`.
- The 8×8 font is U+0020 to U+007E, the same table as display's console (`lib/gfx/font8x8.c`).
- Scalar on every CPU.

**BUGS**
None known.

**SEE ALSO**
[InitRastPort](#gfxinitrastport).
