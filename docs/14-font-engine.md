# 14. OpenType and TrueType font engine (`font.library`)

**Status: proposed (2026-10-20).** Required by `gfx.library`, `ui.library` and the [Workbench desktop environment](13-workbench.md) in V8 (the roadmap's replan after V5).

Planar renders proportional UI typography (`System Sans`) and terminal/code text (`System Mono`) through a clean-room user-mode shared library, **`font.library`**. Rather than porting FreeType and HarfBuzz—which together exceed 250,000 lines of C and C++ covering legacy formats and every world script—Planar implements its own OpenType/TrueType engine **from scratch in C23 directly from the specification documents**.

The engine implements **only the tables and layout stages needed for English typography to look right**. Non-English script shaping, bidirectional reordering, vertical layout, variable font interpolation and colour font formats are defined as explicit, typed stub boundaries so a motivated team that needs them can drop them in later without restructuring `font.library` or its callers. Existing open-source libraries (FreeType and HarfBuzz) are used **strictly on the build host as a differential test oracle**, never as a source of code or internal design.

---

## 14.1 Design principles

1. **Specification-driven, clean-room implementation.**
   All parsing, table validation, shaping and rasterisation in `lib/font/` are written from the primary standards:
   - **ISO/IEC 14496-22:2019** (*Open Font Format*) / **Microsoft OpenType Specification v1.9.1**.
   - **Apple TrueType Reference Manual** (for `glyf`, `loca`, `cmap`, `head`, `hhea`, `hmtx` and `kern`).
   - **Adobe Technical Note #5176 / #5177** (for `CFF ` Type 2 charstrings in `.otf` files).
   No source files, headers, lookup tables or internal data structures may be copied or translated from FreeType, HarfBuzz, `stb_truetype` or `fontdue`.
2. **Zero-copy over shared Areas (P2).**
   A font file loaded by `dos` or embedded via `#embed` ([§7.3](07-language-toolchain.md#73-c23-baseline)) sits in a read-only `Area` shared across protection domains (`ShareArea` with `AREAF_READ`). `font.library` parses tables in place by reading big-endian fields directly out of the mapped `Area` without copying or unpacking the file into heap trees.
3. **Font files are untrusted external input (F2, [§12.4](12-let-it-crash.md#124-the-process-is-the-unit-of-failure)).**
   A malformed `.ttf` or `.otf` file must never fault `font.library` or crash the caller's domain. Every table offset, slice length, glyph index, contour point index, composite component recursion depth and `GSUB`/`GPOS` subtable offset is bounds-checked against the `Area` span before dereferencing. Any violation returns `ERR_BADFONT`.
4. **English-complete, everything else stubbed cleanly.**
   Proportional English UI and editorial text looks degraded if it lacks kerning (`VA`, `To`, `We`), standard ligatures (`fi`, `fl`, `ffi`), tabular digit switching (`tnum` for live counters like the Workbench `CPU 0.8%` HUD badge), or accurate analytical anti-aliasing. `font.library` implements those paths in full and routes non-English and colour features through documented stub hooks.
5. **Oracle-verified on the host, measured on the board (P8).**
   Correctness is enforced in CI by comparing `font.library`'s metrics, shaped positions, outlines and rasterised alpha masks against HarfBuzz + FreeType in a host-only test harness (`tools/font-oracle/`).

---

## 14.2 Architecture and memory model

`font.library` runs in the caller's protection domain from the global shared `RX` library region ([§2.1](02-architecture.md#21-layers)) and is called through its interface table (`IFont->ShapeRun(...)`, `IFont->RasterizeGlyph(...)`, [§7.2](07-language-toolchain.md#72-decision)).

```
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ Font File Area (RO, shared across all processes: SYS:Fonts/System.ttf)  │
 └───────────────────────────────────┬─────────────────────────────────────┘
                                     │ zero-copy big-endian slice reads
                                     ▼
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ font.library (runs in caller's domain; pure C23 + RVV / NEON SIMD)      │
 │                                                                         │
 │  1. SFNT directory & bounds validator                                   │
 │  2. Codepoint mapping:  cmap (Format 4, Format 12)                      │
 │  3. English shaping:    GSUB (liga, tnum, pnum) + GPOS (kern) + kern    │
 │                         ├─▶ [Stub: shaper_complex_stub (non-Latin)]     │
 │                         └─▶ [Stub: bidi_stub (RTL / UAX #9)]            │
 │  4. Outline extractor:  glyf/loca (quadratic) · CFF (cubic)             │
 │                         ├─▶ [Stub: color_font_stub (COLR/SVG/sbix)]     │
 │                         └─▶ [Stub: var_font_stub (fvar/gvar/HVAR)]      │
 │  5. Rasteriser:         Analytical area-coverage scanline (A8 / LCD)    │
 │                         └─▶ [Stub: tt_vm_stub (TrueType bytecode)]      │
 └─────────────────────────────────────────────────────────────────────────┘
```

### Shared glyph atlas (`intuition` fast path)

To avoid rasterising common UI sizes (`12 px`, `13 px`, `16 px`) in every client process, `intuition` (or a small font cache service) can pre-rasterise the ASCII + Latin punctuation working set of `System Sans` and `System Mono` into a read-only shared `Area` (`FontAtlas`). Clients map the `FontAtlas` read-only and blit pre-rendered `A8` glyphs directly via `gfx.library`, invoking the rasteriser in their own domain only for custom sizes, custom fonts, or cache misses.

---

## 14.3 Implemented subsystem scope (English layout profile)

### 1. Character repertoire

The supported character set covers everything needed for English UI, documentation, technical prose and terminal output:

- **Basic Latin (`U+0020`–`U+007E`)**: ASCII letters, digits, punctuation and symbols.
- **Latin-1 Supplement (`U+00A0`–`U+00FF`)**: Non-breaking space, `§`, `©`, `®`, `°`, `±`, `×`, `÷`, currencies (`£`, `¥`, `¢`), and accented loanwords used in English (`é`, `ï`, `ö`, `ü`, `ç`, `ñ`).
- **General Punctuation & Typography (`U+2010`–`U+203A`, `U+20AC`, `U+2122`)**: Hyphens, figure dash, en dash (`–`), em dash (`—`), left/right single and double curly quotes (`‘` `’` `“` `”`), dagger (`†`), bullet (`•`), ellipsis (`…`), narrow no-break space, euro (`€`) and trademark (`™`).
- **Box Drawing & Block Elements (`U+2500`–`U+259F`)** and **Replacement Character (`U+FFFD`)**: Required by `Shell` ([§13.5](13-workbench.md#3-shell-command-line-and-diagnostics)) and diagnostic tools.

### 2. Required OpenType / TrueType tables

| Table | Spec section (ISO 14496-22) | Implemented subset |
|---|---|---|
| **SFNT Header** | §4.2–4.3 | TrueType (`0x00010000`, `'true'`) and OpenType CFF (`'OTTO'`) directory parsing; 4-byte tag binary search; table offset + length bounds verification. Font collections (`ttcf`) are rejected with `ERR_UNSUPPORTED` unless index 0 is requested. |
| `head` | §5.2.1 | `unitsPerEm` (validated in `[16, 16384]`), `indexToLocFormat` (short 16-bit vs. long 32-bit `loca`), global `xMin`/`yMin`/`xMax`/`yMax`. |
| `maxp` | §5.2.4 | v0.5 (CFF) and v1.0 (TrueType): `numGlyphs`, max points, max contours, max composite recursion depth (capped at 16 regardless of table value). |
| `cmap` | §5.2.3 | **Format 4** (segment mapping to delta values for the BMP) and **Format 12** (segmented coverage for full Unicode scalar values). Prefers `(platformID=3, encodingID=10)` then `(3, 1)` then `(0, *)`. |
| `hhea` & `hmtx` | §5.2.2, §5.2.3 | `ascent`, `descent`, `lineGap`, `numberOfHMetrics`, per-glyph `advanceWidth` and `leftSideBearing` (with last-metric repetition for monospaced tails). |
| `OS/2` | §5.2.6 | Versions 0–5: `sTypoAscender`, `sTypoDescender`, `sTypoLineGap`, `usWinAscent`, `usWinDescent`, `sxHeight`, `sCapHeight`, `usWeightClass`, `usWidthClass`, and `fsSelection` bit 7 (`USE_TYPO_METRICS`). |
| `post` & `name` | §5.2.7, §5.2.5 | `post`: `italicAngle`, `underlinePosition`, `underlineThickness`, `isFixedPitch`. `name`: English (`langID=0x0409` or platform 0) strings for Family (`nameID=1, 16`) and Subfamily (`nameID=2, 17`). |
| `loca` & `glyf` | §5.3.1, §5.3.2 | **Simple glyphs**: flag expansion (`REPEAT_FLAG`, short/same X and Y vectors), delta-decoded contour points, on-curve and quadratic off-curve (`conic`) Bézier points, implicit midpoints between consecutive off-curve points. **Composite glyphs**: `ARGS_ARE_WORDS`, `ARGS_ARE_XY_VALUES`, `WE_HAVE_A_SCALE`, `WE_HAVE_AN_X_AND_Y_SCALE`, `WE_HAVE_A_TWO_BY_TWO`, `MORE_COMPONENTS`. |
| `CFF ` | §5.4.1 | Compact Font Format v1 INDEX structures, Top DICT, Private DICT, Local/Global Subrs, and Type 2 charstring interpreter (stems, moves, lines, cubic curves, `endchar`, `callsubr`/`callgsubr` with recursion cap 10). |
| `kern` | §5.6.1 | Legacy TrueType kerning table **Format 0** (sorted glyph-pair binary search) used as a fallback when `GPOS` has no `kern` feature. |
| `GDEF` | §6.2.4 | Glyph Class Definition table (`GlyphClassDef`: base, ligature, mark, component) to respect `IgnoreMarks` lookup flags in `GSUB`/`GPOS`. |
| `GSUB` | §6.2.5 | Script tags `latn` and `DFLT`; language tags `ENG ` and `dflt`. **Lookup Type 1** (Single Substitution Format 1 & 2, for `tnum`/`pnum` figure styles), **Lookup Type 4** (Ligature Substitution Format 1, for `liga`: `fi`, `fl`, `ffi`, `ffl`), and **Lookup Type 7** (Extension Substitution wrapper). |
| `GPOS` | §6.2.6 | Script tags `latn` and `DFLT`. **Lookup Type 2** (Pair Adjustment Positioning Format 1 [individual glyph pairs] and Format 2 [class-based pair matrix], for `kern` horizontal `XAdvance` / `XPlacement` adjustments), and **Lookup Type 9** (Extension Positioning wrapper). |

### 3. Shaping and line-breaking pipeline (`ShapeRun`)

For an English UTF-8 string, `IFont->ShapeRun` executes five deterministic, allocation-free stages into a caller-supplied `GlyphRun` buffer:

1. **UTF-8 decode & sanitise**: Decodes scalar values; invalid byte sequences emit `U+FFFD`.
2. **Script & BiDi check**: Scans codepoints. If any scalar falls into a non-Latin complex script or strong RTL block, invokes the `bidi_stub` / `shaper_complex_stub` hook (§14.4).
3. **Nominal glyph mapping**: Maps each scalar to a 16-bit `GlyphID` via `cmap` (`0` / `.notdef` if missing).
4. **OpenType feature application (`GSUB` → `GPOS`)**:
   - Applies enabled `GSUB` lookups (`liga` on by default for proportional text, off for `System Mono`; `tnum` enabled on HUD badges and tables).
   - Populates initial `x_advance` from `hmtx`.
   - Applies `GPOS` `kern` lookups (or falls back to `kern` Format 0) to adjust adjacent pair advances and placements.
5. **Line measurement & word wrap**:
   - Computes line height from `OS/2` (`sTypoAscender - sTypoDescender + sTypoLineGap`) when `fsSelection & USE_TYPO_METRICS` is set, else `hhea`.
   - Breaks lines at ASCII space (`U+0020`), tab (`U+0009`), newline (`U+000A`), hyphen-minus (`U+002D`) and soft hyphen (`U+00AD`, inserting the visible hyphen glyph only when the break is taken).

*Built (V8 step 2):* `ShapeRun`, `FitLine` and `LineSpacing` ([autodoc](../manuals/autodocs/font.md)). Stage 1 gives a U+FFFD for each maximal subpart of an ill-formed sequence (Unicode §3.9). Stage 3 also gives a space the font lacks its space glyph at Unicode's width, and hides controls and default ignorables (the space glyph, no advance) after stage 4, which passes over ignorables, so a soft hyphen doesn't stop a kern; ZWNJ stops a ligature. A line also breaks at the other newlines (VT, FF, NEL, U+2028, U+2029, a CR alone; CR LF is one), at U+2010 and after a ZWSP; tab stops are every eight spaces.

### 4. Analytical scanline rasteriser

`IFont->RasterizeGlyph` converts a glyph's outlines into an 8-bit linear coverage bitmap (`A8`):

1. **Curve flattening**: Decomposes quadratic (`glyf`) and cubic (`CFF `) Béziers into monotonic line segments using recursive midpoint subdivision with a flatness tolerance of `0.125 px` in device space. *Built (V8 step 1):* evenly spaced points within `1/512 px`, the tolerance it took to be within 2/255 of the exact coverage; at `0.125 px` a pixel on a curve's edge was up to a tenth out. The rasteriser is gfx.library's `PathMask`, which font.library's outlines are drawn by ([v8-plan](v8-plan.md) decision 6).
2. **Exact analytical area integration**: For each scanline $y$, accumulates signed trapezoidal pixel coverage and winding deltas across touched pixels (the same geometric integral used by FreeType's smooth rasteriser and `stb_truetype` v2), clamped by the non-zero winding rule (`CFF ` and standard OpenType) or even-odd rule when requested.
3. **SIMD sweep**: The cumulative winding prefix-sum and `uint8_t` conversion across a scanline use RVV 1.0 on RISC-V and NEON on AArch64 ([§7.4](07-language-toolchain.md#74-simd-policy)), with a portable C23 scalar implementation verified alongside it.
4. **Gamma-correct blending**: `gfx.library` blends the `A8` coverage mask in linear light (via a 256-entry sRGB-to-linear lookup table) so light text on dark slate (`pane.dark.bg`, `#36435A`) and dark text on brushed silver (`pane.light.bg`, `#D8DEE8`) maintain equal perceived stem weight.

---

## 14.4 Stubbed subsystems and extension boundaries

All out-of-scope OpenType subsystems have fixed function signatures, error codes and dispatch hooks in `lib/font/stubs.c`. When `font.library` encounters a table or script that requires a stubbed subsystem, it sets a warning flag in `GlyphRun.flags` and takes a safe fallback so rendering continues without crashing.

| Stubbed subsystem | Tables / features skipped | Stub entry point in `lib/font/stubs.c` | Default fallback behaviour |
|---|---|---|---|
| **Non-Latin & complex script shaping** | `GSUB` Lookup Types 2, 3, 5, 6, 8 (contextual, chaining contextual, reverse chaining); `GPOS` Lookup Types 1, 3, 4, 5, 6, 7, 8 (cursive attachment, mark-to-base, mark-to-ligature, mark-to-mark); `MORX`/`AAT`; Arabic/Indic/SEA cluster state machines. | `font_shape_complex_stub(font, script_tag, run)` | Maps codepoints 1:1 through `cmap` and advances by `hmtx` without reordering or contextual substitution; sets `FONT_WARN_STUB_SHAPER`. |
| **Bidirectional text (UAX #9)** | RTL scripts (Arabic, Hebrew, Syriac, Thaana) and BiDi embedding overrides (`U+202A`–`U+202E`, `U+2066`–`U+2069`). | `font_bidi_reorder_stub(codepoints, count, levels)` | Keeps logical left-to-right visual order; sets `FONT_WARN_STUB_BIDI`. |
| **Vertical layout & CJK breaking** | `vhea`, `vmtx`, `VORG`, `BASE`, `JSTF`, vertical `GSUB` features (`vert`, `vrt2`), and UAX #14 East Asian line-break rules. | `font_vertical_metrics_stub(font, glyph_id, out)` | Returns `ERR_UNSUPPORTED`; line wrapper breaks only on whitespace and hyphens. |
| **Colour fonts & embedded bitmaps** | `COLR`/`CPAL` (v0 & v1), `SVG `, `CBDT`/`CBLC`, `sbix`, `EBDT`/`EBLC`. | `font_raster_color_stub(font, glyph_id, ppem, out)` | Returns `ERR_UNSUPPORTED` to the caller, which falls back automatically to the monochrome vector outline in `glyf` or `CFF ` and sets `FONT_WARN_STUB_COLOR`. |
| **Variable fonts** | `fvar`, `gvar`, `HVAR`, `VVAR`, `MVAR`, `STAT`, `avar`, and `CFF2` blend operators. | `font_apply_variations_stub(font, coords, count)` | Ignores variation deltas and renders the font's default master outlines from `glyf`/`CFF `; sets `FONT_WARN_STUB_VAR`. |
| **TrueType bytecode hinting VM** | `fpgm`, `prep`, `cvt `, `gasp`, and per-glyph instruction bytecode in `glyf`. | `font_tt_hint_glyph_stub(font, glyph_id, ppem, pts)` | No-op; passes unhinted scaled coordinates straight to the analytical rasteriser. |

A team adding support for any of these subsystems replaces the corresponding function in `lib/font/stubs.c` (or registers a handler table) and adds an oracle test suite for that script or table in `tools/font-oracle/`.

---

## 14.5 Open-source test oracle (`tools/font-oracle/`)

### Role of open-source libraries

FreeType (`libfreetype`) and HarfBuzz (`libharfbuzz`) are **test oracles only**:

- **Never linked into Planar**: Neither library lives in the repo, builds for `rv64` or `aarch64` targets, or ships in any Planar image.
- **No implementation borrowing**: `lib/font/` does not copy FreeType/HarfBuzz structs, state machines or tables.
- **Host-side differential verification (`make test-font-oracle`)**: On the build host (`plat/hosted`), a standalone test runner in `tools/font-oracle/` compiles `lib/font/*.c` into a host binary and links the host OS's system `libfreetype` and `libharfbuzz` side by side.

```
                   ┌──────────────────────────────┐
                   │ Font corpus (.ttf / .otf)    │
                   │ + English text & UI corpus   │
                   └──────────────┬───────────────┘
                                  │
                 ┌────────────────┴────────────────┐
                 ▼                                 ▼
  ┌──────────────────────────────┐  ┌──────────────────────────────┐
  │ Planar lib/font/ (under test)│  │ Host Oracle                  │
  │ Written from ISO 14496-22    │  │ HarfBuzz + FreeType          │
  │ (no hinting, analytical A8)  │  │ (FT_LOAD_NO_HINTING)         │
  └──────────────┬───────────────┘  └──────────────┬───────────────┘
                 │                                 │
                 └───────────▶ Compare ◀───────────┘
                                  │
         1. Global & glyph metrics:   exact bit match
         2. Shaped GlyphIDs & (x,y):  exact ID, ≤ 1 design unit
         3. Flattened outline pts:    exact in font units
         4. Raster A8 alpha masks:    max pixel diff ≤ 2/255, ≥ 99.8% match
```

### Oracle verification passes

`make test-font-oracle` runs five automated checks across a reference font corpus (including TrueType `.ttf` and CFF `.otf` fonts in regular, bold, italic and monospace weights):

1. **Pass 1 — Table & Metric Oracle**:
   Queries `unitsPerEm`, ascender, descender, line gap, `numGlyphs`, and every glyph's `advanceWidth` and `leftSideBearing` from both `lib/font/` and FreeType (`FT_LOAD_NO_SCALE`). Requires an **exact bit-for-bit match** across all glyphs in the corpus.
2. **Pass 2 — Shaping & Kerning Oracle**:
   Shapes an English test corpus—including kerning torture pairs (`"To WA VA Yo We Av T. LT P. F, Y. f” ‘A’"`), ligature sequences (`"fi fl ffi ffl ff"`), punctuation runs (`"12:38 PM — CPU 0.8% • RAM 0.00% …"`) and tabular-numeral strings (`tnum` on and off)—through both `IFont->ShapeRun` and `hb_shape()` (configured with `HB_SCRIPT_LATIN`, `hb_language_from_string("en")`, and matching `{kern, liga, tnum}` feature toggles).
   - Output `GlyphID` arrays must match **100%**.
   - Per-glyph `x_advance`, `x_offset` and `y_offset` in font design units must match **100%**, or within **$\le 1$ pixel** when scaled to 26.6 fixed-point device coordinates.
   - *Built (V8 step 2):* glyphs, clusters and drawn positions match exactly, over 3,487 texts in 11 fonts, with every default feature HarfBuzz has and `ShapeRun` hasn't turned off. Not compared, because they differ on purpose: ill-formed UTF-8 (HarfBuzz gives a U+FFFD a byte), controls (hidden), and a hidden glyph's position ([v8-plan](v8-plan.md) step 2).
3. **Pass 3 — Outline Decomposition Oracle**:
   Extracts unpacked contour points and composite transformations for every printable English glyph and compares them against `FT_Outline_Decompose` in font design units. Requires exact coordinate parity (or $\le 1$ font unit on 2×2 fixed-point `F2Dot14` composite transforms).
4. **Pass 4 — Analytical Raster Oracle**:
   Renders all printable English glyphs at `12 px`, `13 px`, `16 px`, `24 px` and `48 px` using `IFont->RasterizeGlyph` and compares the resulting `A8` buffers against FreeType (`FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL`, `FT_RENDER_MODE_NORMAL`):
   - Bounding box (`bitmap_left`, `bitmap_top`, `width`, `rows`) must match within $\le 1\text{ px}$.
   - Maximum per-pixel coverage difference across shared interior pixels must be $\le 2 / 255$ (fixed-point tie-breaking tolerance), with $\ge 99.8\%$ of pixels within $\le 1 / 255$.
   - *Built (V8 step 1):* these numbers hold against the **exact** coverage, which the oracle computes itself, not against FreeType's. FreeType's unhinted renderer approximates curves more coarsely (up to 29/255 from the exact on a curve's edge), so it can't be matched to 2/255 without copying its flattening (§14.1). Against FreeType the oracle checks the boxes and guards set from measurement ([v8-plan](v8-plan.md) step 1).
5. **Pass 5 — Adversarial Table Fuzzer (`tools/font-fuzz`)**:
   Mutates table offsets, `cmap` segment arrays, composite glyph component indices (including self-referential cycles `A → B → A`), `CFF ` subroutine calls and `GPOS` class matrices under AddressSanitizer and UndefinedBehaviorSanitizer. Every corrupted input must return `ERR_BADFONT` or `ERR_UNSUPPORTED` with zero out-of-bounds reads, zero leaks and bounded execution time.

---

## 14.6 Public C23 API surface (`include/planar/font.h`)

**Proposed.** Exposed via `font.library`'s interface table `IFont`:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum : int32_t {
    FONT_OK              =  0,
    ERR_BADFONT          = -40,  // Malformed or truncated SFNT/table data
    ERR_UNSUPPORTED      = -41,  // Collection or stubbed table with no fallback
    ERR_NOGLYPH          = -42,  // Codepoint unmapped in cmap
} font_err_t;

typedef enum : uint32_t {
    FONT_FEAT_KERN       = 1u << 0,  // GPOS 'kern' or legacy 'kern' (default on)
    FONT_FEAT_LIGA       = 1u << 1,  // GSUB 'liga' standard ligatures (default on)
    FONT_FEAT_TNUM       = 1u << 2,  // GSUB 'tnum' tabular numerals
    FONT_FEAT_PNUM       = 1u << 3,  // GSUB 'pnum' proportional numerals
} font_feat_flags_t;

typedef enum : uint32_t {
    FONT_WARN_NONE        = 0,
    FONT_WARN_STUB_SHAPER = 1u << 0, // Non-Latin script hit shaper_complex_stub
    FONT_WARN_STUB_BIDI   = 1u << 1, // RTL codepoint hit bidi_reorder_stub
    FONT_WARN_STUB_COLOR  = 1u << 2, // Colour table skipped; rendered monochrome
    FONT_WARN_STUB_VAR    = 1u << 3, // Variable axes skipped; rendered default master
} font_warn_flags_t;

typedef struct {
    uint16_t units_per_em;
    int16_t  ascender;       // Typographic ascender (font design units)
    int16_t  descender;      // Typographic descender (negative, font units)
    int16_t  line_gap;       // Typographic line gap (font units)
    int16_t  x_height;
    int16_t  cap_height;
    uint16_t num_glyphs;
    bool     is_fixed_pitch;
    bool     is_cff;
} FontMetrics;

typedef struct {
    uint16_t glyph_id;
    uint16_t cluster;        // Byte offset into the input UTF-8 string
    int16_t  x_advance;      // Design units (or 26.6 device px when scaled)
    int16_t  x_offset;
    int16_t  y_offset;
} ShapedGlyph;

typedef struct {
    ShapedGlyph      *glyphs;
    size_t            capacity;
    size_t            count;
    int32_t           total_advance;
    font_warn_flags_t warnings;
} GlyphRun;

typedef struct {
    uint8_t *pixels;         // Caller-supplied A8 coverage buffer (stride * rows)
    int32_t  width;
    int32_t  rows;
    int32_t  stride;
    int32_t  bearing_x;      // Left bearing in integer device pixels
    int32_t  bearing_y;      // Top bearing above baseline in device pixels
} GlyphBitmap;
```

Core interface operations on `struct FontIFace`:

- `[[nodiscard]] font_err_t (*OpenFontArea)(const void *area_base, size_t area_len, FontFace **out_face);`
- `void (*GetMetrics)(const FontFace *face, FontMetrics *out);`
- `uint16_t (*MapCodepoint)(const FontFace *face, uint32_t codepoint);`
- `[[nodiscard]] font_err_t (*ShapeRun)(const FontFace *face, const char *utf8, size_t len, uint32_t feat_flags, GlyphRun *inout_run);`
- `[[nodiscard]] font_err_t (*MeasureBitmap)(const FontFace *face, uint16_t glyph_id, float ppem, float subpixel_x, GlyphBitmap *out_box);`
- `[[nodiscard]] font_err_t (*RasterizeGlyph)(const FontFace *face, uint16_t glyph_id, float ppem, float subpixel_x, GlyphBitmap *inout_bmp);`

---

## 14.7 Implementation notes: third-party C shortcut candidates

Writing `font.library` entirely from scratch from ISO 14496-22 (§14.1 #1)—including the `CFF ` Type 2 charstring VM, `GSUB`/`GPOS` table walkers, and analytical rasteriser—is estimated at **4,000–6,000+ LOC** (comparable to the 10–15k LOC budget of the entire kernel in [§7.1.1](07-language-toolchain.md#711-why-c23-over-newer-systems-languages)).

If Principle 1 (§14.1) is relaxed to allow adapting small, permissive C libraries behind `struct FontIFace` (§14.6) while keeping the zero-copy `Area` model (§14.2), stub boundaries (§14.4), and FreeType/HarfBuzz host oracle (§14.5), three existing C candidates stand out:

| Requirement in `14-font-engine.md` | **`libschrift`** | **`stb_truetype.h`** | **`kb_text_shape.h`** |
|---|---|---|---|
| **Approximate size** | **~1,450 LOC** (`schrift.c` + `.h`) | **~3,500 LOC** code (~5k w/ comments) | **~22,000 LOC** (single header) |
| **License** | ISC | Public Domain (Unlicense) / MIT | MIT / Public Domain |
| **Zero-copy over RO `Area` (§14.1 #2)** | Yes (reads `const uint8_t *` in place) | Yes (`stbtt_InitFont` points into buffer) | Yes |
| **`cmap` Format 4 & 12 (§14.3.2)** | Yes (Format 4, 6, 12) | Yes (Format 0, 2, 4, 6, 12, 13) | Yes (shaping only, no rasteriser) |
| **TrueType outlines (`glyf`/`loca`)** | Yes (simple + composite) | Yes (simple + composite) | No (shaper only) |
| **OpenType cubic outlines (`CFF `)** | **No** (`.ttf` only) | **Yes** (Type 2 charstring interpreter) | No |
| **Analytical A8 scanline rasteriser** | **Yes** (trapezoidal coverage + subpixel) | **Yes** (v2 analytical coverage + subpixel) | No |
| **Kerning (`kern` + `GPOS` Lookup 2)** | `kern` Format 0 only | **Both** (`kern` Format 0 + `GPOS` Pair Adjustment Fmt 1 & 2 via `stbtt_GetGlyphGPOSInfoAdvance`) | Full `GPOS` |
| **`GSUB` (`liga`, `tnum`, `pnum`)** | No | No | Full `GSUB` |
| **Bounds checking on malformed fonts** | **High** (checks offsets against `size`; returns `-1`) | **Low** (trusts internal offsets; needs `area_len` bounds checks added) | Medium |

### Candidate breakdown

1. **Option A — `stb_truetype.h` (~3,500 LOC; ~2,200 LOC stripped):**
   - Already implements ~85% of this document: TrueType `glyf`/`loca`, OpenType `CFF ` Type 2 charstrings (`stbtt__run_charstring`), `cmap` Formats 4 & 12, analytical A8 rasterisation with subpixel offsets (`stbtt_MakeGlyphBitmapSubpixel`), and both `kern` Format 0 and `GPOS` Lookup Type 2 (Format 1 & 2 class-based pair kerning via `stbtt_GetGlyphGPOSInfoAdvance`).
   - **Gaps to close:** Add ~250 LOC for `GSUB` Lookup Type 1 (`tnum`/`pnum`) and Lookup Type 4 (`liga`); thread `area_len` bounds checks into table/offset reads so Pass 5 fuzzing (§14.5) returns `ERR_BADFONT` cleanly; strip the v1 rasteriser, SDF generator, and SVG helpers to bring it down to ~2,200 LOC.
2. **Option B — `libschrift` (~1,450 LOC):**
   - Clean, modern imperative C with explicit bounds checking on every big-endian read (`offset + n <= font->size`), stack/caller scratch buffers, and the same analytical trapezoidal area-coverage algorithm as `stb_truetype` v2 with subpixel positioning.
   - **Gaps to close:** Supports `.ttf` (`glyf`/`loca`) only—no `.otf` (`CFF `) cubic outlines (acceptable if base fonts ship as `.ttf` per VQ34). Only parses legacy `kern` Format 0, so it must be paired with ~400–500 LOC for `GPOS` Lookup Type 2 (ported from `stbtt_GetGlyphGPOSInfoAdvance` or written from spec) and `GSUB` Lookup Types 1 & 4.
3. **Option C — `kb_text_shape.h` (~22,000 LOC):**
   - Single-header C shaper covering `GSUB` and `GPOS`, but exceeds the entire kernel LOC budget because it bundles full Unicode segmentation tables and complex-script state machines that §14.4 deliberately stubs out. Not recommended as a drop-in.

### Recommended hybrid shortcut (~1,900–2,500 LOC total)

- **If `.ttf` only is sufficient (per VQ34):** Base `lib/font/` on **`libschrift`** (~1,450 LOC) for bounds-checked SFNT/table parsing, `cmap`, `glyf`/`loca`, and analytical A8 rasterisation + port **`stbtt_GetGlyphGPOSInfoAdvance`** (~180 LOC, bounds-hardened) for `GPOS` kerning + write ~250 LOC for **`GSUB` Lookup 1 (`tnum`/`pnum`) and Lookup 4 (`liga`)**. Total: **~1,900 LOC**.
- **If `.otf` (`CFF `) support is required:** Base `lib/font/` on a stripped, bounds-checked **`stb_truetype.h`** (~2,200 LOC) + ~250 LOC for **`GSUB` Lookup 1 & 4**. Total: **~2,500 LOC**.

---

## 14.8 Open questions

- **VQ34 Base system font files (`System Sans` and `System Mono`).**
  Which permissive open-source font files (e.g. OFL-1.1 or Apache-2.0) should ship in the image via `#embed` for V4 (`Shell`) and V6 (`Workbench`)?
  *Leaning:* subsetted `.ttf` builds of an OFL geometric/humanist sans (such as Inter or IBM Plex Sans) and programmer monospace (JetBrains Mono or IBM Plex Mono), stripped of stubbed tables (`gvar`, non-Latin glyphs) for the embedded boot image while full unmodified `.ttf`/`.otf` files sit in the `tools/font-oracle/` test corpus.

- **VQ35 Horizontal subpixel positioning (`1/4 px` bins) vs. whole-pixel glyph placement.**
  At small UI sizes (`12–13 px`) without TrueType bytecode hinting, snapping every glyph's origin to an integer pixel causes uneven inter-letter spacing, whereas `4×` horizontal subpixel phase bins (`0.0`, `0.25`, `0.5`, `0.75 px`) give smooth kerning at the cost of `4×` atlas entries for cached sizes.
  *Leaning:* `4×` horizontal subpixel phase bins for `System Sans` at `12–16 px`, and integer pixel origins (`subpixel_x = 0`) for `System Mono` and sizes $\ge 18\text{ px}$.
