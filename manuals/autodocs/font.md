# font: TrueType fonts read in place

*Checked against V8 step 2.*

font.library's functions, in alphabetical order. The header is `planar/font.h`. They're a shared library's, called through `IFont` (`struct FontIFace`, the "main" interface, version 1, of font.library 8). The same functions by the same names are in `lib/font`, for the host's oracle and fuzzer. None traps, allocates or keeps state: a `FontFace` is the caller's, and points into the font's file, which the caller keeps. Any task can call any of them at any time.

The library reads TrueType outlines (`glyf`) only, never runs hinting, and checks every read against the file's length: a malformed font is `ERR_BADFONT`, never a fault. A glyph's outline is a `GfxPath`; gfx.library's [PathMask](gfx.md#gfxpathmask) turns it into coverage. Sizes are pixels an em in 26.6 fixed point: 13 px is `13 << 6`.

Text is shaped by [ShapeRun](#fontshaperun): UTF-8 to glyphs and where each goes, in the font's units, with its kerning, ligatures and figure styles. [FitLine](#fontfitline) breaks a shaped run into lines, and [LineSpacing](#fontlinespacing) says how far apart they go. Latin only: a run in another script is laid out a glyph a character, and says so.

---

## font/FitLine

**NAME**
FitLine -- the line a shaped run's next glyphs make, in a width

**SYNOPSIS**
```c
#include <planar/font.h>
void FitLine(const struct FontFace *face, struct GlyphRun *run, uint32_t first, int32_t width, uint32_t size,
             struct LineFit *fit);
```

**FUNCTION**
Takes glyphs from `first` on while they fit in `width` at `size`, and breaks where a line may: after a space, a tab, a hyphen or a soft hyphen, or at a newline, which always ends the line. It's greedy: the line is as long as fits. Spaces at a line's end hang past it, and aren't counted in its width. A soft hyphen is a break only if a hyphen fits after it, and then the hyphen is drawn there. A word wider than the line is cut where it stops fitting, and a line always takes one glyph at least.

A tab's advance is set here, in the run (and `gr_Advance` with it): to the next stop, every eight spaces from the line's start.

**INPUTS**
- `face` -- the font the run was shaped in.
- `run` -- from [ShapeRun](#fontshaperun). Its tabs' advances are written.
- `first` -- the line's first glyph: 0, then each line's `first` plus its `lf_Count`.
- `width` -- the line's width, in 26.6 pixels.
- `size` -- pixels an em, 26.6, as the run will be drawn.
- `fit` -- the line:
  - `lf_Count` -- the glyphs it takes, its trailing spaces and newline among them.
  - `lf_Width` -- its width in 26.6 pixels, without the trailing spaces, with a hyphen if it has one.
  - `lf_Flags` -- `LINEF_END` (the run's last line), `LINEF_NEWLINE` (ended by a newline), `LINEF_HYPHEN` (broken at a soft hyphen: draw `lf_Hyphen` at its end), `LINEF_FORCED` (a word cut).
  - `lf_Hyphen` -- the hyphen's glyph.

**RESULT**
None. `first` at the run's end or past it is a line of none, `LINEF_END`.

**EXAMPLE**
```c
struct LineFit fit;

for (uint32_t at = 0; at < run.gr_Count; at += fit.lf_Count) {
    IFont->FitLine(&face, &run, at, 300 << 6, 13 << 6, &fit);
    ...     /* glyphs at to at + lf_Count, a line down each time */
}
```

**NOTES**
- **Planar's own.**
- `tools/font-oracle` breaks a paragraph at 53 widths in two fonts, and checks every line fits, ends where a line may, and couldn't have taken more.
- A line's last glyph keeps the kerning it had with the next line's first, perhaps a few of the font's units.

**BUGS**
Breaks are §14.3.3's, not Unicode's line breaking (UAX #14): no break after an en dash or a slash, or before an opening bracket.

**SEE ALSO**
[ShapeRun](#fontshaperun), [LineSpacing](#fontlinespacing).

---

## font/FontInfo

**NAME**
FontInfo -- a font's metrics

**SYNOPSIS**
```c
#include <planar/font.h>
void FontInfo(const struct FontFace *face, struct FontMetrics *m);
```

**FUNCTION**
Fills in `*m` from the font's `head`, `maxp`, `hhea`, `OS/2` and `post` tables, in the font's units: units an em, glyphs, `hhea`'s ascender, descender and line gap, `OS/2`'s typographic and Windows ones, its x-height and cap height (version 2 on), weight and `fsSelection`, `head`'s box of every glyph, and `post`'s italic angle, underline and fixed pitch.

**INPUTS**
- `face` -- an open font.
- `m` -- where the metrics go.

**RESULT**
None.

**EXAMPLE**
```c
struct FontMetrics m;

IFont->FontInfo(&face, &m);
line = IFont->ScaleUnits(&face, m.fm_Ascender - m.fm_Descender + m.fm_LineGap, 13 << 6);
```

**NOTES**
- **Planar's own:** the Amiga's `TextFont` was a bitmap font's; this is an outline font's.
- A field whose table is missing or too old is 0.
- Each field is FreeType's for the same font (`tools/font-oracle`, pass 1).

**BUGS**
None known.

**SEE ALSO**
[OpenFont](#fontopenfont), [ScaleUnits](#fontscaleunits), [GetGlyphMetrics](#fontgetglyphmetrics).

---

## font/FontName

**NAME**
FontName -- one of a font's names, in English

**SYNOPSIS**
```c
#include <planar/font.h>
int64_t FontName(const struct FontFace *face, uint32_t nameId, char *buffer, uint64_t size);
```

**FUNCTION**
Finds name `nameId` in the font's `name` table, preferring the Windows platform's US English record, then any Windows Unicode one, then the Unicode platform's, then Macintosh Roman's. Copies it to `buffer` as ASCII, any other character as `?`, cut to fit and ended with a NUL.

**INPUTS**
- `face` -- an open font.
- `nameId` -- `FONTNAME_FAMILY` (1), `FONTNAME_SUBFAMILY` (2), `FONTNAME_TYPO_FAMILY` (16), `FONTNAME_TYPO_SUBFAMILY` (17), or another of the table's IDs.
- `buffer`, `size` -- where it goes. `size` may be 0, to ask the length.

**RESULT**
The name's length in characters, which may be more than was copied; `ERR_NOTFOUND` if the font hasn't that name; `ERR_BADFONT` if its record points outside the table.

**EXAMPLE**
```c
char family[32];

IFont->FontName(&face, FONTNAME_FAMILY, family, sizeof family);    /* "Inter" */
```

**NOTES**
- **Planar's own.**

**BUGS**
None known.

**SEE ALSO**
[FontInfo](#fontfontinfo).

---

## font/GetGlyphMetrics

**NAME**
GetGlyphMetrics -- a glyph's advance and left side bearing

**SYNOPSIS**
```c
#include <planar/font.h>
int64_t GetGlyphMetrics(const struct FontFace *face, uint32_t glyph, struct GlyphMetrics *m);
```

**FUNCTION**
Reads the glyph's advance width and left side bearing from `hmtx`, in the font's units: past the table's last full entry, its last advance and the bearings after it.

**INPUTS**
- `face` -- an open font.
- `glyph` -- a glyph index, below `fm_NumGlyphs`.
- `m` -- where they go.

**RESULT**
0, or `ERR_BADARG` for a glyph past the last.

**EXAMPLE**
```c
struct GlyphMetrics gm;

IFont->GetGlyphMetrics(&face, IFont->MapCodepoint(&face, 'W'), &gm);
x += IFont->ScaleUnits(&face, gm.gm_Advance, 13 << 6);
```

**NOTES**
- **Planar's own.**
- [ShapeRun](#fontshaperun)'s advances are kerned; this is the glyph's own.

**BUGS**
None known.

**SEE ALSO**
[MapCodepoint](#fontmapcodepoint), [ScaleUnits](#fontscaleunits), [ShapeRun](#fontshaperun).

---

## font/GlyphOutline

**NAME**
GlyphOutline -- a glyph's outline, scaled, as a path

**SYNOPSIS**
```c
#include <planar/font.h>
int64_t GlyphOutline(const struct FontFace *face, uint32_t glyph, uint32_t size, struct GfxPath *path);
```

**FUNCTION**
Empties `path`, then writes the glyph's outline into it at `size`: its contours as lines and quadratics, in 26.6 fixed point, y down, the origin on the baseline at the glyph's origin. A composite glyph's components are placed by their offsets and matrices. An on-curve point between two off-curve ones is implied halfway, as TrueType has it. Each contour ends with `PATH_CLOSE`.

**INPUTS**
- `face` -- an open font.
- `glyph` -- a glyph index, below `fm_NumGlyphs`.
- `size` -- pixels an em, 26.6: from 1/64 to 4,096 px.
- `path` -- its arrays, and their sizes in `gp_MaxPoints` and `gp_MaxVerbs`. A glyph needs at most about twice its points and contours.

**RESULT**
0, with the outline in `path`: an empty one for a glyph with none (a space). Otherwise `path` is left empty and:
- `ERR_FULL` -- the arrays are too small;
- `ERR_BADARG` -- the glyph or the size is out of range;
- `ERR_BADFONT` -- the glyph's data is malformed, or a composite holds itself, nests more than 16 deep, or has more than 2,048 components in all;
- `ERR_UNSUPPORTED` -- a component placed by matching points (§14.4's stub).

**EXAMPLE**
```c
static struct GfxPoint pts[1024];
static uint8_t verbs[1024];
struct GfxPath path = { pts, verbs, 1024, 1024, 0, 0 };

if (IFont->GlyphOutline(&face, g, 13 << 6, &path) == 0 && IGfx->PathBounds(&path, &r))
    ...     /* PathMask */
```

**NOTES**
- **Planar's own.**
- Points are scaled as FreeType scales them unhinted, to the 1/64 px (`tools/font-oracle`, pass 3), and in font units they're the file's own.
- No hinting is run: TrueType's instructions are skipped (§14.4).

**BUGS**
None known.

**SEE ALSO**
[PathBounds](gfx.md#gfxpathbounds), [PathMask](gfx.md#gfxpathmask), [MapCodepoint](#fontmapcodepoint).

---

## font/LineSpacing

**NAME**
LineSpacing -- how far apart a font's lines go

**SYNOPSIS**
```c
#include <planar/font.h>
void LineSpacing(const struct FontFace *face, struct LineSpacing *ls);
```

**FUNCTION**
Fills in the font's ascent, descent and line gap, and their sum, the distance from one baseline to the next, in the font's units. They're `OS/2`'s typographic ones if its `USE_TYPO_METRICS` bit is set (and each isn't 0), else `hhea`'s (§14.3.3).

**INPUTS**
- `face` -- an open font.
- `ls` -- where they go: `ls_Ascent` above the baseline, `ls_Descent` below it (positive), `ls_LineGap`, and `ls_Height`, the three added.

**RESULT**
None.

**EXAMPLE**
```c
struct LineSpacing ls;

IFont->LineSpacing(&face, &ls);
y += IFont->ScaleUnits(&face, ls.ls_Height, 13 << 6);
```

**NOTES**
- **Planar's own.**
- As HarfBuzz's `hb_font_get_h_extents` reads them (`tools/font-oracle`, pass 2).

**BUGS**
None known.

**SEE ALSO**
[FitLine](#fontfitline), [FontInfo](#fontfontinfo).

---

## font/MapCodepoint

**NAME**
MapCodepoint -- the glyph for a character

**SYNOPSIS**
```c
#include <planar/font.h>
uint32_t MapCodepoint(const struct FontFace *face, uint32_t codepoint);
```

**FUNCTION**
Looks the Unicode code point up in the font's `cmap`: a format 12 subtable if the font has one for Unicode, else format 4.

**INPUTS**
- `face` -- an open font.
- `codepoint` -- a Unicode scalar value.

**RESULT**
The glyph index, or 0 (`.notdef`) if the font has none for it.

**EXAMPLE**
```c
uint32_t g = IFont->MapCodepoint(&face, 0x2014);       /* an em dash */
```

**NOTES**
- **Planar's own.**
- Every code point from 0 to U+10FFFF maps as FreeType maps it (`tools/font-oracle`, pass 1).

**BUGS**
None known.

**SEE ALSO**
[GlyphOutline](#fontglyphoutline), [GetGlyphMetrics](#fontgetglyphmetrics).

---

## font/OpenFont

**NAME**
OpenFont -- check a font's tables and open it

**SYNOPSIS**
```c
#include <planar/font.h>
int64_t OpenFont(struct FontFace *face, struct Span file);
```

**FUNCTION**
Reads the font's table directory and checks what the other calls rely on: that `head`, `maxp`, `hhea`, `hmtx`, `loca` and `glyf` are there and long enough, `head`'s magic, units an em from 16 to 16,384, a glyph count, and a `cmap` subtable whose arrays fit. Fills in `*face`, which then points into `file`.

**INPUTS**
- `face` -- the caller's, filled in.
- `file` -- the font file in memory, read-only: from [SystemFont](#fontsystemfont), or loaded. It must outlive the face.

**RESULT**
0, or:
- `ERR_BADFONT` -- not a TrueType font, or one cut short or malformed;
- `ERR_UNSUPPORTED` -- a CFF font (`OTTO`) or a collection (`ttcf`).

**EXAMPLE**
```c
struct FontFace face;
struct Span file;

if (IFont->SystemFont(FONT_SANS, &file) && IFont->OpenFont(&face, file) == 0)
    ...
```

**NOTES**
- **Differs from Exec:** graphics.library's `OpenFont` took a `TextAttr` and found a bitmap font in memory. This opens a file the caller has.
- Nothing needs closing: the library holds nothing.
- A font with no `cmap` opens, and maps every character to `.notdef`.

**BUGS**
None known.

**SEE ALSO**
[SystemFont](#fontsystemfont), [FontInfo](#fontfontinfo).

---

## font/ScaleUnits

**NAME**
ScaleUnits -- the font's units at a size, in 26.6 pixels

**SYNOPSIS**
```c
#include <planar/font.h>
int32_t ScaleUnits(const struct FontFace *face, int32_t units, uint32_t size);
```

**FUNCTION**
Scales `units` of the font to pixels at `size`, as its outlines are scaled: by `size / unitsPerEm` in 16.16 fixed point, rounded.

**INPUTS**
- `face` -- an open font.
- `units` -- a length in the font's units.
- `size` -- pixels an em, 26.6.

**RESULT**
The length in 26.6 pixels.

**EXAMPLE**
```c
int32_t advance = IFont->ScaleUnits(&face, gm.gm_Advance, 13 << 6);   /* 26.6 */
```

**NOTES**
- **Planar's own.**

**BUGS**
None known.

**SEE ALSO**
[FontInfo](#fontfontinfo), [GetGlyphMetrics](#fontgetglyphmetrics).

---

## font/ShapeRun

**NAME**
ShapeRun -- text to glyphs, and where each goes

**SYNOPSIS**
```c
#include <planar/font.h>
int64_t ShapeRun(const struct FontFace *face, const char *text, uint64_t len, uint32_t features,
                 struct GlyphRun *run);
```

**FUNCTION**
Shapes `len` bytes of UTF-8 into the caller's array (§14.3.3):
1. Decodes it. Each ill-formed part (a cut sequence, an overlong one, a surrogate) is one U+FFFD, as Unicode's §3.9 says: the longest part that could have begun a character.
2. Maps each character to a glyph through `cmap`. A space the font hasn't got is its space glyph at the width Unicode gives it: an em over 2 for an en space, over 5 for a thin one, a figure's for a figure space, half a space for a narrow no-break one. A no-break hyphen the font hasn't got is U+2010's glyph.
3. Applies `GSUB`'s lookups for the features asked for: `liga`'s ligatures, `tnum`'s and `pnum`'s figures, each behind an extension or not.
4. Gives each glyph its advance from `hmtx`, then applies `GPOS`'s `kern` (pair adjustments, by glyph or by class), or the `kern` table's format 0 if `GPOS` has no `kern`.

A lookup passes over the glyphs its flags say to (GDEF's classes), and over default ignorables (a soft hyphen, ZWSP, ZWJ), so "A", soft hyphen, "V" is kerned as "AV". ZWNJ stops a ligature.

Controls and default ignorables stay in the run, so that a line can break at them, but are hidden (`GLYPHF_HIDDEN`): the space glyph, with no advance. Each glyph's flags say what it is to [FitLine](#fontfitline): `GLYPHF_SPACE`, `GLYPHF_TAB`, `GLYPHF_NEWLINE` (CR LF is one, the LF's), `GLYPHF_HYPHEN`, `GLYPHF_SOFT_HYPHEN`.

A character in a script that needs more than Latin's shaping (Hebrew, Arabic, Devanagari, combining marks) makes the whole run one glyph a character with `hmtx`'s advances, no lookups, and `gr_Warnings` says so: `FONT_WARN_STUB_SHAPER`, and `FONT_WARN_STUB_BIDI` too if it's written right to left (§14.4). A variable font is shaped as its default, with `FONT_WARN_STUB_VAR`; one with colour glyphs is shaped, with `FONT_WARN_STUB_COLOR`.

**INPUTS**
- `face` -- an open font.
- `text`, `len` -- UTF-8. `len` under 2 GB.
- `features` -- `FONT_FEAT_KERN`, `FONT_FEAT_LIGA`, `FONT_FEAT_TNUM`, `FONT_FEAT_PNUM`, or'd: `FONT_FEAT_KERN | FONT_FEAT_LIGA` for prose, `FONT_FEAT_KERN | FONT_FEAT_TNUM` for figures in columns. No other feature is applied.
- `run` -- `gr_Glyphs` and `gr_Max`, the caller's: one `ShapedGlyph` a character is the most needed. Filled in:
  - each glyph's `sg_Glyph`, `sg_Cluster` (the byte its first character starts at; a ligature's is its first's), `sg_Advance`, `sg_XOffset` and `sg_YOffset` (from the pen, y up), and `sg_Flags`, all in the font's units;
  - `gr_Count`, `gr_Advance` (the advances' sum: the run's width) and `gr_Warnings`.

**RESULT**
0, or, with `gr_Count` 0:
- `ERR_FULL` -- more characters than `gr_Max`;
- `ERR_BADARG` -- `len` 2 GB or more;
- `ERR_BADFONT` -- a layout table that points outside itself, or a substitute past the last glyph;
- `ERR_UNSUPPORTED` -- more than 256 lookups for the features asked.

**EXAMPLE**
```c
static struct ShapedGlyph glyphs[256];
struct GlyphRun run = { glyphs, 256, 0, 0, 0 };

if (IFont->ShapeRun(&face, "Waffle", 6, FONT_FEAT_KERN | FONT_FEAT_LIGA, &run) == 0)
    width = IFont->ScaleUnits(&face, run.gr_Advance, 13 << 6);
```

**NOTES**
- **Planar's own.** The RKM's `Text` took bytes in a bitmap font; gfx.library draws a shaped run (V8 step 3).
- The glyphs, their clusters and where each is drawn are HarfBuzz's for the same text and features, exactly, over the corpus (`tools/font-oracle`, pass 2), but for these, on purpose: HarfBuzz gives a U+FFFD a byte of ill-formed UTF-8, and controls the font's own glyphs; ShapeRun hides controls.
- Lookup types not built (contextual, chained, marks, cursive) are passed over, with `FONT_WARN_STUB_SHAPER`.
- Allocates nothing.

**BUGS**
No Unicode normalisation: a character the font hasn't got isn't decomposed into one it has.

**SEE ALSO**
[FitLine](#fontfitline), [LineSpacing](#fontlinespacing), [MapCodepoint](#fontmapcodepoint), [ScaleUnits](#fontscaleunits).

---

## font/SystemFont

**NAME**
SystemFont -- one of the system's fonts

**SYNOPSIS**
```c
#include <planar/font.h>
bool SystemFont(uint32_t which, struct Span *file);
```

**FUNCTION**
Gives the file of one of the four fonts font.library carries among its constants: System Sans (Inter) and System Mono (JetBrains Mono), Regular and Bold, each cut to Latin, punctuation, box drawing and blocks (§14.3.1). Every process that opens the library maps the same copy.

**INPUTS**
- `which` -- `FONT_SANS` or `FONT_MONO`, with `FONT_BOLD` for the bold.
- `file` -- where its span goes.

**RESULT**
True, or false for a `which` there's no font for.

**EXAMPLE**
```c
struct Span file;

IFont->SystemFont(FONT_MONO | FONT_BOLD, &file);
```

**NOTES**
- **Planar's own.**
- Both fonts are under the SIL Open Font License 1.1, its text with the originals in `tools/font-oracle/corpus/`. `tools/font-subset.py` makes the subsets.
- System Sans has no box drawing, no U+FFFD and no `liga` feature (Inter has none); System Mono has no kerning, being monospaced.

**BUGS**
None known.

**SEE ALSO**
[OpenFont](#fontopenfont).
