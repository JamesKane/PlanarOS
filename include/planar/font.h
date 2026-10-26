/*
 * planar/font.h -- font.library: TrueType fonts read in place
 *
 * V8 step 1 (docs §14). A font is a TrueType file in memory the caller
 * can read: one of the two system fonts the library carries (SystemFont),
 * or one from elsewhere, read-only. OpenFont checks its tables and fills
 * in a FontFace, the caller's: the library keeps nothing, allocates
 * nothing, and never writes the file. Every read is checked against the
 * file's length, and anything malformed is ERR_BADFONT, never a fault.
 *
 * What a font gives: its metrics, a glyph for a character (cmap formats
 * 4 and 12), a glyph's advance and bearing, and its outline as a GfxPath
 * (planar/gfx.h), scaled and y down, which gfx.library turns into
 * coverage (PathMask). font.library doesn't rasterise (v8-plan decision
 * 6). TrueType outlines only: a CFF font ('OTTO') or a collection is
 * ERR_UNSUPPORTED (decision 1). Hinting is never run.
 *
 * Sizes are pixels an em, in 26.6 fixed point (16 px is 16 << 6).
 * Metrics are in the font's units unless said otherwise.
 */
#ifndef PLANAR_FONT_H
#define PLANAR_FONT_H

#include <stdint.h>

#include <planar/exec.h>
#include <planar/gfx.h>
#include <planar/utility.h>

constexpr int64_t ERR_BADFONT = -40;        /* malformed, or a table cut short */
constexpr int64_t ERR_UNSUPPORTED = -41;    /* CFF, a collection, or a stub's (§14.4) */

/* SystemFont's: the face, and FONT_BOLD for its bold. */
enum : uint32_t {
    FONT_SANS = 0,              /* System Sans: Inter */
    FONT_MONO = 1,              /* System Mono: JetBrains Mono */
};
constexpr uint32_t FONT_BOLD = 1u << 8;

/* An open font: the caller's, filled in by OpenFont, read by the rest.
 * It points into the file, which must outlive it. */
struct FontFace {
    uint64_t ff_Private[48];
};

struct FontMetrics {
    uint16_t fm_UnitsPerEm;
    uint16_t fm_NumGlyphs;
    int16_t fm_Ascender, fm_Descender, fm_LineGap;      /* hhea's */
    int16_t fm_TypoAscender, fm_TypoDescender, fm_TypoLineGap;  /* OS/2's; 0 if none */
    uint16_t fm_WinAscent, fm_WinDescent;               /* OS/2's */
    int16_t fm_XHeight, fm_CapHeight;                   /* OS/2 v2's; 0 before */
    uint16_t fm_Weight;                                 /* usWeightClass: 400 regular */
    uint16_t fm_FsSelection;                            /* bit 7: USE_TYPO_METRICS */
    int16_t fm_XMin, fm_YMin, fm_XMax, fm_YMax;         /* head's, every glyph's box */
    int16_t fm_UnderlinePosition, fm_UnderlineThickness;
    int32_t fm_ItalicAngle;                             /* degrees, 16.16 */
    uint32_t fm_FixedPitch;                             /* post's isFixedPitch: not 0 */
};

struct GlyphMetrics {
    uint16_t gm_Advance;                                /* hmtx's advance width */
    int16_t gm_LeftBearing;
};

/* FontName's: the name table's IDs. */
enum : uint32_t {
    FONTNAME_FAMILY = 1,
    FONTNAME_SUBFAMILY = 2,
    FONTNAME_TYPO_FAMILY = 16,
    FONTNAME_TYPO_SUBFAMILY = 17,
};

/*
 * font.library's "main" interface, version 1 (V8 step 1), in this order,
 * which is the ABI:
 *
 *     struct Library *lib = IExec->OpenLibrary("font.library", 8);
 *     struct FontIFace *IFont =
 *         lib ? (struct FontIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
 */
struct FontIFace {
    struct Interface Data;

    bool (*SystemFont)(uint32_t which, struct Span *file);
    int64_t (*OpenFont)(struct FontFace *face, struct Span file);
    void (*FontInfo)(const struct FontFace *face, struct FontMetrics *m);
    int64_t (*FontName)(const struct FontFace *face, uint32_t nameId, char *buffer, uint64_t size);
    uint32_t (*MapCodepoint)(const struct FontFace *face, uint32_t codepoint);
    int64_t (*GetGlyphMetrics)(const struct FontFace *face, uint32_t glyph, struct GlyphMetrics *m);
    int64_t (*GlyphOutline)(const struct FontFace *face, uint32_t glyph, uint32_t size,
                            struct GfxPath *path);
    int32_t (*ScaleUnits)(const struct FontFace *face, int32_t units, uint32_t size);
};

/* lib/font's own names, which the interface's entries are. */
bool SystemFont(uint32_t which, struct Span *file);
/* The file's tables checked and found: 0, ERR_BADFONT or ERR_UNSUPPORTED. */
int64_t OpenFont(struct FontFace *face, struct Span file);
void FontInfo(const struct FontFace *face, struct FontMetrics *m);
/* The English name 'nameId' (platform 3, or 0, or 1's Roman), as ASCII,
 * anything else '?': its length, or ERR_NOTFOUND. Cut to fit, with a NUL. */
int64_t FontName(const struct FontFace *face, uint32_t nameId, char *buffer, uint64_t size);
/* The glyph for a character, or 0 (.notdef) if the font has none. */
uint32_t MapCodepoint(const struct FontFace *face, uint32_t codepoint);
int64_t GetGlyphMetrics(const struct FontFace *face, uint32_t glyph, struct GlyphMetrics *m);
/* The glyph's outline at 'size' (26.6 px an em) into 'path', which it
 * empties first: 26.6 fixed point, y down, its origin on the baseline.
 * ERR_FULL if the path's arrays are too small, ERR_BADFONT, or
 * ERR_UNSUPPORTED (a composite placed by matching points). */
int64_t GlyphOutline(const struct FontFace *face, uint32_t glyph, uint32_t size, struct GfxPath *path);
/* 'units' of the font at 'size', in 26.6 pixels: as its outlines scale. */
int32_t ScaleUnits(const struct FontFace *face, int32_t units, uint32_t size);

#endif /* PLANAR_FONT_H */
