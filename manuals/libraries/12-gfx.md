# 12. gfx.library: bitmaps and drawing

*Written, V7 step 2; paths, gradients and type, V8 step 3.*

gfx.library draws on the CPU into memory a program owns: rectangles, blits with alpha, bevels, anti-aliased paths and gradients, and text in the system's fonts. It's the RKM's graphics.library without bitplanes, Chip RAM or the blitter. A bitmap is 32-bit pixels in an Area, and every operation runs in the caller's own process, with no message to anyone ([docs §16.4](../../docs/16-rtg-display-gpu.md)). intuition (V7 step 3) composes windows from bitmaps a client drew this way, and display's buffers can be drawn in with it too.

It's a shared library (chapter 8), reached through `IGfx`. Its functions are in `planar/gfx.h` and `lib/gfx`, also in the archive `libgfx.a` for a program that wants them linked in, as the tests do. It keeps no state: the bitmaps and rastports are the caller's.

## Concepts

- **A `BitMap`** describes pixels in memory: the address of the top left one (`bm_BaseAddr`), the width and height, and the bytes from one row to the next (`bm_BytesPerRow`). Rows follow one another (`MOD_LINEAR`), and the colour space is sRGB.
- **Pixels** are 32 bits, blue in the low byte: `0xAARRGGBB` as a `uint32_t`, as display's formats are.
  - `PIXFMT_XRGB8888` is opaque, and its top byte isn't read.
  - `PIXFMT_ARGB8888` has alpha in the top byte, not premultiplied.
- **`AllocBitMap`** makes a bitmap in an Area of its own, owned by the calling process. The pixels start at the Area's start, rows are a multiple of 64 bytes apart, and the `BitMap` itself sits after the last row. So the Area is everything: `FreeBitMap` frees it, and sharing it (`ShareArea` of `bm_Area`) shares the description with the pixels.
  - With `BMF_DISPLAYABLE` and a **friend** bitmap, the Area is placed as the friend's is: not cached (`AREAF_NC`) or below 4 GiB (`AREAF_DMA32`) if the friend's Area is. A bitmap as wide as its friend takes the friend's row stride.
- **`InitBitMap`** describes memory the program already has, a display buffer say. It owns no Area, and `FreeBitMap` leaves it alone.
- **A `RastPort`** is where to draw:
  - its bitmap;
  - a clip rectangle, inclusive, which is clipped again to the bitmap;
  - a foreground and a background pen, which are pixels;
  - the draw mode for text, `JAM1` or `JAM2`;
  - the font, where `nullptr` is the 8×8.
  
  `InitRastPort` sets one up: the whole bitmap, white on black, `JAM2`, the 8×8 font.
- **Coordinates are signed.** A rectangle may start off the bitmap or run past it, and only what's inside the clip is drawn. Rectangles are inclusive (`xMin` to `xMax`), as the RKM's `RectFill` was.
- **Chosen at boot.** The drawing calls each have a scalar entry and one by the vector unit (RVV 1.0 on RISC-V, NEON on Arm). As the kernel relocates the library, it points the table at the vector unit's where the CPU has one, as it does exec.library's `CopyMem`. The two give the same pixels to the bit. A bitmap font's text is scalar on both: 8 pixels to a row leave a vector unit nothing to do.
- **A path** (V8) is the caller's arrays of points and verbs, in 26.6 fixed point (1/64 px), y down: lines, quadratic and cubic Béziers. `PathMask` turns one into **coverage**, a byte a pixel, which `BltMask` blends in a pen. `FillPath` does both, and `FillGradient` fills with a gradient instead of a pen.
- **An outline font** (V8) is one of font.library's system fonts at a size, opened by `OpenTextFont` into an Area of the caller's that holds its glyph cache. A rastport draws in it after `SetFont`.

## Drawing

From `conformance/gfx.c`, shortened:
```c
#include <planar/gfx.h>

struct Library *lib = IExec->OpenLibrary("gfx.library", 7);     /* 8 for V8's calls */
struct GfxIFace *IGfx = lib ? (struct GfxIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
struct BitMap *bm = IGfx->AllocBitMap(64, 48, PIXFMT_ARGB8888, 0, nullptr);
struct RastPort rp;

IGfx->InitRastPort(&rp, bm);
rp.rp_FgPen = 0x80c08040;
IGfx->RectFill(&rp, 0, 0, 63, 47);
IGfx->DrawBevel(&rp, 2, 2, 61, 45, 0xffffffff, 0xff404040);
rp.rp_FgPen = 0xff000000;
rp.rp_DrawMode = JAM1;
IGfx->Text(&rp, 8, 20, "shared", 6);
/* ... */
IGfx->FreeBitMap(bm);
```

`BltBitMap` copies a rectangle from a bitmap into a rastport:
- `BLEND_SRC` copies the pixels as they are.
- `BLEND_SRC_OVER` blends an ARGB source over the destination, taking the destination as opaque. Each colour channel becomes `(s × a + d × (255 − a)) / 255`, rounded to the nearest. Alpha becomes `a + da × (255 − a) / 255`, rounded the same way. An XRGB source is opaque, so it's copied.

A blit within one bitmap may overlap its source in any direction. The result is as though the whole source were read before anything was written.

`DrawBevel` draws the light from the top left ([docs §13.2](../../docs/13-workbench.md)). The top and left edges are 1 pixel of `light`. The bottom and right edges are 1 pixel of `dark`, which takes the top right and bottom left corners.

`Text` draws `count` characters from the top left at (x, y), each `tf_XSize` pixels on from the last. Set bits take the foreground pen. In `JAM2` clear ones take the background pen, and in `JAM1` they're left alone. A character the font doesn't hold is drawn as its first glyph (the 8×8's space).

## Paths and gradients

A path's points are the caller's: `gp_Points` and `gp_Verbs`, with their sizes. `PathRoundRect` adds a rounded rectangle; anything else is written in directly, a verb and its points at a time (`PATH_MOVE` and `PATH_LINE` one point, `PATH_QUAD` two, `PATH_CUBIC` three, `PATH_CLOSE` none). From `conformance/typescene.h`, a ring and a bar:
```c
static struct GfxPoint pts[48];
static uint8_t verbs[48];
struct GfxPath path = { pts, verbs, 48, 48, 0, 0 };
struct GfxGradient bar = { { 8, 0, 151, 0 }, 0xffd8dee8, 0xff8bb4e8, GRADF_HORIZONTAL };

IGfx->PathRoundRect(&path, &(struct GfxRect){ 8, 6, 151, 30 }, 6 * 64);
IGfx->FillGradient(&rp, &bar, &path, 0);

path.gp_NumPoints = path.gp_NumVerbs = 0;
IGfx->PathRoundRect(&path, &(struct GfxRect){ 110, 52, 150, 92 }, 20 * 64);
IGfx->PathRoundRect(&path, &(struct GfxRect){ 120, 62, 140, 82 }, 10 * 64);
rp.rp_FgPen = 0xffe0a040;
IGfx->FillPath(&rp, &path, PATHF_EVENODD);
```

- **Coverage is exact to 2/255.** Curves are cut into lines within 1/512 px of them, and each line's area is added up pixel by pixel, in integers, so every CPU gives the same bytes. The fill rule is non-zero winding, or even-odd with `PATHF_EVENODD`.
- **It's blended in linear light** ([docs §14.3.4](../../docs/14-font-engine.md)). Each channel of the pen and the pixel goes to 16 bits of linear light by a table, the two are mixed by the coverage, and the result comes back to sRGB by another. Blending sRGB values directly would make light text on a dark ground look thinner than dark text on a light one; this keeps them the same weight. It's within 1/255 of the exact blend for every pen, pixel and coverage.
- **FillPath works in pieces.** Only the path's box within the clip is worked on, as much at a time as its scratch holds: 2.5 KB of the stack, or the rastport's `TmpRas` if it has one (`InitTmpRas`), as the RKM's `AreaEnd` had. The pieces join without a seam, but each walks the whole path again, so give a rastport that fills big paths a `TmpRas` of a few tens of KB.
- **A gradient** goes from one colour at one edge of its rectangle to another at the other edge, down or across, each channel in a straight line in sRGB. Without a path it fills the rectangle; with one, the path.

## Type

An outline font is opened at a size, set in a rastport, drawn in, and closed. From `conformance/typetest.c`:
```c
struct TextFont *tf = IGfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 });

rp.rp_FgPen = 0xffe8eef6;
rp.rp_DrawMode = JAM1;
IGfx->SetFont(&rp, tf);
IGfx->Text(&rp, 8, 36, "Type, at last", 13);
/* ... */
IGfx->SetFont(&rp, nullptr);
IGfx->CloseTextFont(tf);
```

- **The fonts.** `"System Sans"` (Inter) or `"System Mono"` (JetBrains Mono), 1 to 255 pixels an em, and `FSF_BOLD` for either's bold. `tf_YSize` is the line's height, `tf_Baseline` the line's top to its baseline, and System Mono's `tf_XSize` its advance.
- **`Text`** takes UTF-8 in an outline font. It's shaped by font.library (chapter 14): kerned, and ligated in System Sans. It's drawn with the line's top left at (x, y), as in a bitmap font, so the baseline is `y + tf_Baseline`. `TextLength` says how wide it will be.
- **`TextRun`** draws glyphs of a run already shaped, on a baseline: for a paragraph shaped once and broken into lines by font.library's `FitLine`, or for text drawn more than once.
- **The glyph cache.** A glyph is rasterised the first time it's drawn, and blended from the cache after. System Sans at 12 to 16 px is placed to a quarter pixel, which evens out its spacing at small sizes, so a glyph can be in the cache four times. Every other font and size is placed to a whole pixel. The cache is in the font's Area: it's the process's own, nothing is shared between processes, and closing the font, or the process dying, frees it.
- **gfx.library opens font.library itself**, through the program's `IExec`: a program that draws text needn't open font.library unless it shapes text of its own for `TextRun`.

## Drawing in display's buffers

A buffer from display's `DISPLAY_ALLOC` is memory the client has mapped, so `InitBitMap` describes it. From `conformance/displaytest.c`:
```c
struct BitMap screen;
struct RastPort rp;

IGfx->InitBitMap(&screen, (void *)a->px, a->width, a->height, a->stride, PIXFMT_XRGB8888);
IGfx->InitRastPort(&rp, &screen);
/* ... draw, then DISPLAY_COMMIT the buffer to plane 1 ... */
```

## Costs

Measured by `conformance/gfx.c` under QEMU (TCG), a megapixel at a time. They're QEMU's costs, not the hardware's: TCG runs most vector instructions an element at a time, so the blend by RVV or NEON is slower here than the scalar loop it replaces. The boards' are below.

| | RV64 | AArch64 |
|---|---|---|
| `RectFill`, scalar | 1.07 ms | 1.50 ms |
| `RectFill`, by the table | 1.18 ms | 0.37 ms |
| `BltBitMap` `BLEND_SRC_OVER`, scalar | 3.8 ms | 3.9 ms |
| `BltBitMap` `BLEND_SRC_OVER`, by the table | 9.0 ms | 5.9 ms |
| `Text`, a glyph | 354 ns | 318 ns |

V8's, from `conformance/typetest.c` under QEMU, by the table:

| | RV64 | AArch64 |
|---|---|---|
| a System Sans glyph at 13 px, rasterised into the cache | 69 µs | 18 µs |
| a line of 80 of them from the cache | 1.78 ms | 0.36 ms |
| a 200 px rounded rectangle, `FillPath` without a `TmpRas` | 7.2 ms | 0.95 ms |

On the boards (V7 step 6, a megapixel at a time), where the vector unit's entries are the faster ones:

| | the RV2 (X60, 256-bit RVV) | the CIX (A720, NEON) |
|---|---|---|
| `RectFill`, scalar | 1,283 µs | 546 µs |
| `RectFill`, by the table | 529 µs (2.4×) | 84 µs (6.5×) |
| `BltBitMap` `BLEND_SRC_OVER`, scalar | 29.8 ms | 5.96 ms |
| `BltBitMap` `BLEND_SRC_OVER`, by the table | 6.8 ms (4.4×) | 1.08 ms (5.5×) |
| `Text`, a glyph | 715 ns | 94 ns |

## Rules

- **Check what `AllocBitMap` returns.** It's `nullptr` for a size of 0 or over `GFX_MAX_SIZE` (16384), for a format or flag it doesn't know, or when there's no memory.
- **Don't free what `AllocBitMap` didn't make.** `FreeBitMap` frees the Area its `BitMap` names, and the `BitMap` with it: don't touch it after.
- **A bitmap shared is all shared.** Its `BitMap` is in its Area, so whoever it's shared with reads the description too, and can trust it no more than the pixels.
- **Pens are pixels.** Give a pen the alpha you want written: `RectFill` writes the pen as it is, top byte included.
- **The vector unit's first use costs a trap**, as exec.library's `CopyMem` does: the kernel switches its state lazily.
- **A `TextFont` and a `TmpRas` are one task's at a time**, as a `RastPort` is: two tasks drawing text at once each open their own font.
- **Close a font after its last use.** `CloseTextFont` frees the `TextFont` with its Area; a rastport still set to it would draw from freed memory.
- **Shape and draw in the same face.** A run given to `TextRun` must have been shaped in the rastport's font's face and style.

## Differences from the RKM

| graphics.library | gfx.library |
|---|---|
| Bitplanes, a depth, and Chip RAM | 32-bit pixels in an Area, anywhere |
| `AllocBitMap(w, h, depth, flags, friend)` | `AllocBitMap(w, h, format, flags, friend)`: a pixel format, not a depth |
| Minterms (`0xC0`) | `BLEND_SRC` and `BLEND_SRC_OVER` (Porter-Duff) |
| Pens index a palette (`SetAPen`) | Pens are pixels, set in the `RastPort` |
| `Text` draws at the baseline (`Move` first) | At the line's top left given, in either kind of font; `TextRun` takes a baseline |
| `OpenFont` finds a bitmap font, `OpenDiskFont` loads one | `OpenTextFont` opens an outline font at any size, and caches its glyphs |
| `AreaMove`, `AreaDraw`, `AreaEnd` with an `AreaInfo` and a `TmpRas` | A `GfxPath` of the caller's, filled anti-aliased by `FillPath`; the `TmpRas` optional |
| `BltTemplate` draws a 1-bit template | `BltMask` blends 8-bit coverage, in linear light |
| Layers clip a `RastPort` | One clip rectangle. intuition composes windows, so nothing draws over another's |
| `OwnBlitter`, `WaitBlit` | Nothing to wait for: the CPU draws, and the call returns when it's done |

## Functions used

[AllocBitMap](../autodocs/gfx.md#gfxallocbitmap), [BltBitMap](../autodocs/gfx.md#gfxbltbitmap), [BltMask](../autodocs/gfx.md#gfxbltmask), [DrawBevel](../autodocs/gfx.md#gfxdrawbevel), [FillGradient](../autodocs/gfx.md#gfxfillgradient), [FillPath](../autodocs/gfx.md#gfxfillpath), [FreeBitMap](../autodocs/gfx.md#gfxfreebitmap), [GetBitMapAttr](../autodocs/gfx.md#gfxgetbitmapattr), [InitBitMap](../autodocs/gfx.md#gfxinitbitmap), [InitRastPort](../autodocs/gfx.md#gfxinitrastport), [InitTmpRas](../autodocs/gfx.md#gfxinittmpras), [OpenTextFont](../autodocs/gfx.md#gfxopentextfont), [PathBounds](../autodocs/gfx.md#gfxpathbounds), [PathMask](../autodocs/gfx.md#gfxpathmask), [PathRoundRect](../autodocs/gfx.md#gfxpathroundrect), [RectFill](../autodocs/gfx.md#gfxrectfill), [SetFont](../autodocs/gfx.md#gfxsetfont), [Text](../autodocs/gfx.md#gfxtext), [TextLength](../autodocs/gfx.md#gfxtextlength), [TextRun](../autodocs/gfx.md#gfxtextrun); [OpenLibrary](../autodocs/exec.md#execopenlibrary), [ShareArea](../autodocs/exec.md#execsharearea); [ShapeRun](../autodocs/font.md#fontshaperun), [FitLine](../autodocs/font.md#fontfitline).
