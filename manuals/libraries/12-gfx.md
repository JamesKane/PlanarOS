# 12. gfx.library: bitmaps and drawing

*Written, V7 step 2.*

gfx.library draws on the CPU into memory a program owns: rectangles, blits with alpha, bevels and text. It's the RKM's graphics.library without bitplanes, Chip RAM or the blitter. A bitmap is 32-bit pixels in an Area, and every operation runs in the caller's own process, with no message to anyone ([docs §16.4](../../docs/16-rtg-display-gpu.md)). intuition (V7 step 3) composes windows from bitmaps a client drew this way, and display's buffers can be drawn in with it too.

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
- **Chosen at boot.** `RectFill`, `BltBitMap` and `DrawBevel` each have a scalar entry and one by the vector unit (RVV 1.0 on RISC-V, NEON on Arm). As the kernel relocates the library, it points the table at the vector unit's where the CPU has one, as it does exec.library's `CopyMem`. The two give the same pixels to the bit. `Text` is scalar on both: 8 pixels to a row leave a vector unit nothing to do.

## Drawing

From `conformance/gfx.c`, shortened:
```c
#include <planar/gfx.h>

struct Library *lib = IExec->OpenLibrary("gfx.library", 7);
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

## Differences from the RKM

| graphics.library | gfx.library |
|---|---|
| Bitplanes, a depth, and Chip RAM | 32-bit pixels in an Area, anywhere |
| `AllocBitMap(w, h, depth, flags, friend)` | `AllocBitMap(w, h, format, flags, friend)`: a pixel format, not a depth |
| Minterms (`0xC0`) | `BLEND_SRC` and `BLEND_SRC_OVER` (Porter-Duff) |
| Pens index a palette (`SetAPen`) | Pens are pixels, set in the `RastPort` |
| `Text` draws at the baseline (`Move` first) | At the top left given, until V8's fonts give a baseline |
| Layers clip a `RastPort` | One clip rectangle. intuition composes windows, so nothing draws over another's |
| `OwnBlitter`, `WaitBlit` | Nothing to wait for: the CPU draws, and the call returns when it's done |

## Functions used

[AllocBitMap](../autodocs/gfx.md#gfxallocbitmap), [BltBitMap](../autodocs/gfx.md#gfxbltbitmap), [DrawBevel](../autodocs/gfx.md#gfxdrawbevel), [FreeBitMap](../autodocs/gfx.md#gfxfreebitmap), [GetBitMapAttr](../autodocs/gfx.md#gfxgetbitmapattr), [InitBitMap](../autodocs/gfx.md#gfxinitbitmap), [InitRastPort](../autodocs/gfx.md#gfxinitrastport), [RectFill](../autodocs/gfx.md#gfxrectfill), [Text](../autodocs/gfx.md#gfxtext); [OpenLibrary](../autodocs/exec.md#execopenlibrary), [ShareArea](../autodocs/exec.md#execsharearea).
