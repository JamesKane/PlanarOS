# 14. font.library: fonts, shaping and lines

*Written, V8 step 3.*

font.library reads TrueType fonts in place and turns text into glyphs: which glyph each character is, where each goes, how a run of them breaks into lines, and how far apart the lines are. It doesn't draw. A glyph's outline is a path, which gfx.library turns into pixels (chapter 12), and gfx.library's `Text` and `TextRun` call this library for a program that only wants text on the screen.

It's a shared library (chapter 8), reached through `IFont`. Its functions are in `planar/font.h` and `lib/font`. It keeps no state and allocates nothing: a face and a run are the caller's. It's Planar's own, written from the OpenType specification ([docs §14](../../docs/14-font-engine.md)), and held on the host to FreeType's metrics and outlines and to HarfBuzz's shaping (`tools/font-oracle`).

## Concepts

- **A font file** is TrueType (`glyf` outlines) in memory the caller can read. The library carries four, among its constants: **System Sans** (Inter) and **System Mono** (JetBrains Mono), Regular and Bold, each cut to Latin, punctuation, box drawing and blocks. `SystemFont` gives one's span. A CFF font or a collection is `ERR_UNSUPPORTED`.
- **A `FontFace`** is a font opened: `OpenFont` checks the file's tables and fills in the caller's `FontFace`, which then points into the file. Nothing is closed: the library holds nothing, so a face is dropped by forgetting it. The file must outlive it.
- **Units.** A font's metrics, advances and kerning are in its own units, `fm_UnitsPerEm` to an em. Sizes are pixels an em in 26.6 fixed point (`13 << 6` is 13 px), and `ScaleUnits` turns units into 26.6 pixels at a size, as outlines are scaled.
- **A `GlyphRun`** is the caller's array of `ShapedGlyph`s, which `ShapeRun` fills: each glyph's number, the byte of the text it came from, its advance, its offset from the pen, and flags that say what it is (a space, a tab, a newline, a hyphen, or hidden).
- **Malformed is an error, never a fault.** Every read is checked against the file's length, every offset, count and recursion. A bad font gives `ERR_BADFONT`, from any call that meets the bad part.

## Opening a font

From `conformance/shapetest.c`:
```c
#include <planar/font.h>

struct Library *lib = IExec->OpenLibrary("font.library", 8);
struct FontIFace *IFont = lib ? (struct FontIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
struct FontFace sans;
struct Span file;

if (IFont->SystemFont(FONT_SANS, &file) && IFont->OpenFont(&sans, file) == 0)
    ...
```

`FontInfo` gives its metrics (`hhea`'s, `OS/2`'s, `head`'s and `post`'s), `FontName` its names, `MapCodepoint` a character's glyph, `GetGlyphMetrics` a glyph's advance and bearing, and `GlyphOutline` its outline at a size, as a `GfxPath`.

## Shaping

Text is UTF-8. `ShapeRun` decodes it, maps each character to a glyph, applies the font's ligatures and figure styles (`GSUB`) for the features asked for, gives each glyph its advance, and kerns (`GPOS`, or the old `kern` table):
```c
static struct ShapedGlyph glyphs[256];
struct GlyphRun run = { glyphs, 256, 0, 0, 0 };

if (IFont->ShapeRun(&sans, "Waffle", 6, FONT_FEAT_KERN | FONT_FEAT_LIGA, &run) == 0)
    width = IFont->ScaleUnits(&sans, run.gr_Advance, 13 << 6);     /* 26.6 px */
```

- **Features.** `FONT_FEAT_KERN | FONT_FEAT_LIGA` for prose; `FONT_FEAT_KERN | FONT_FEAT_TNUM` for figures that line up in columns. Nothing else is applied.
- **A glyph a character is the most** a run needs: make `gr_Max` the text's length in bytes and `ERR_FULL` can't happen.
- **Controls stay in the run, hidden.** A tab, a newline or a soft hyphen is a glyph with `GLYPHF_HIDDEN` and no advance, so that `FitLine` can break at it. A drawer passes hidden glyphs over (gfx.library's `TextRun` does).
- **Latin only.** A run with Hebrew, Arabic, Devanagari or combining marks in it is laid out a glyph a character, in the order written, and `gr_Warnings` says so (`FONT_WARN_STUB_SHAPER`, and `FONT_WARN_STUB_BIDI` for right to left). Nothing fails.

## Lines

`FitLine` takes a shaped run's next line in a width; `LineSpacing` says how far apart lines go:
```c
struct LineSpacing ls;
struct LineFit fit;

IFont->LineSpacing(&sans, &ls);
for (uint32_t at = 0; at < run.gr_Count; at += fit.lf_Count) {
    IFont->FitLine(&sans, &run, at, 300 << 6, 13 << 6, &fit);
    ...     /* glyphs at to at + fit.lf_Count; then down ls.ls_Height units */
}
```

A line breaks after a space, a tab, a hyphen or a soft hyphen, and always at a newline. It's as long as fits. Spaces at its end hang past the width; a soft hyphen taken shows a hyphen (`LINEF_HYPHEN`, `lf_Hyphen`); a word too long for any line is cut (`LINEF_FORCED`). Tab stops are every eight spaces from the line's start, and `FitLine` writes each tab's advance into the run.

## Drawing

gfx.library draws in font.library's fonts (chapter 12). For a label or a title, `Text` in a font from `OpenTextFont` is all it takes: gfx.library shapes it here and draws it. For a paragraph, shape it once, break it with `FitLine`, and draw each line with `TextRun`, in a `TextFont` of the same face, style and size:
```c
struct TextFont *tf = IGfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 });
int32_t y = top + tf->tf_Baseline;

IGfx->SetFont(&rp, tf);
for (uint32_t at = 0; at < run.gr_Count; at += fit.lf_Count, y += tf->tf_YSize) {
    IFont->FitLine(&sans, &run, at, width << 6, tf->tf_Size, &fit);
    IGfx->TextRun(&rp, &run, at, fit.lf_Count, left, y);
}
```

## Rules

- **Keep the file while the face is used.** A face points into it; `SystemFont`'s are the library's constants, and always there.
- **Check every result.** `OpenFont`, `ShapeRun`, `GlyphOutline` and `GetGlyphMetrics` can each say `ERR_BADFONT` for a font that opened: the bad part may be a table only they read.
- **Shape and break at the size you draw.** `FitLine` measures at the size it's given; a run broken at one size and drawn at another won't fit.
- **Draw a run in the face it was shaped in.** Glyph numbers are the face's own.
- **Hidden glyphs aren't drawn,** and a line's count includes its trailing spaces and newline.

## Differences from the RKM

| graphics.library and diskfont.library | font.library |
|---|---|
| Bitmap fonts, a size each, from `FONTS:` (`OpenDiskFont`) | Outline fonts at any size; the system's in the library |
| `OpenFont(textAttr)` finds a font in memory | `OpenFont(face, file)` opens a file the caller has; gfx.library's `OpenTextFont` takes a `TextAttr` |
| `Text` takes bytes, a glyph each | `ShapeRun` takes UTF-8, kerns and ligates, and gfx.library draws the run |
| `TextLength`, `TextFit` | `ScaleUnits` of a run's advance; `FitLine`, which breaks between words |
| `CloseFont` | Nothing to close |

## Functions used

[FitLine](../autodocs/font.md#fontfitline), [FontInfo](../autodocs/font.md#fontfontinfo), [FontName](../autodocs/font.md#fontfontname), [GetGlyphMetrics](../autodocs/font.md#fontgetglyphmetrics), [GlyphOutline](../autodocs/font.md#fontglyphoutline), [LineSpacing](../autodocs/font.md#fontlinespacing), [MapCodepoint](../autodocs/font.md#fontmapcodepoint), [OpenFont](../autodocs/font.md#fontopenfont), [ScaleUnits](../autodocs/font.md#fontscaleunits), [ShapeRun](../autodocs/font.md#fontshaperun), [SystemFont](../autodocs/font.md#fontsystemfont); [OpenTextFont](../autodocs/gfx.md#gfxopentextfont), [Text](../autodocs/gfx.md#gfxtext), [TextRun](../autodocs/gfx.md#gfxtextrun), [PathMask](../autodocs/gfx.md#gfxpathmask).
