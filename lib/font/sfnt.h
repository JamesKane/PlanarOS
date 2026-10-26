/*
 * lib/font/sfnt.h -- font.library's insides: an open font's tables
 *
 * A FontFace is a struct font: spans of the file, one a table, found and
 * checked by OpenFont, and the numbers every call needs. A span that's
 * empty is a table the file hasn't got.
 */
#ifndef LIB_FONT_SFNT_H
#define LIB_FONT_SFNT_H

#include <planar/font.h>

struct font {
    struct Span file;
    struct Span head, maxp, hhea, hmtx, loca, glyf, os2, post, name;
    struct Span cmap;                   /* the subtable used, from its format on */
    struct Span gsub, gpos, gdef, kern; /* step 2's */
    uint32_t cmap_format;               /* 4 or 12; 0: none, so every character is .notdef */
    uint32_t extras;                    /* FONT_HAS_*: tables only a stub would read */
    uint16_t upem, nglyphs, nhmetrics;
    bool loca_long;
};
static_assert(sizeof(struct font) <= sizeof(struct FontFace));

enum : uint32_t {
    FONT_HAS_FVAR = 1u << 0,            /* a variable font */
    FONT_HAS_COLOR = 1u << 1,           /* COLR, CBDT, sbix or SVG */
};

static inline const struct font *font_of(const struct FontFace *face)
{
    return (const struct font *)face->ff_Private;
}

/* 16.16 fixed point, rounded to nearest, halves away from zero. */
int64_t font_mulfix(int64_t a, int64_t b);
int64_t font_divfix(int64_t a, int64_t b);

/*
 * A glyph's points, as the file has them (but scaled, and placed by any
 * composites holding them): on-curve or not, contour by contour. The
 * oracle reads them so; GlyphOutline turns them into a path. 'point'
 * false: stop, the sink is full.
 */
struct font_sink {
    bool (*point)(struct font_sink *s, int32_t x, int32_t y, bool on);
    bool (*contour_end)(struct font_sink *s);
};

/* 'scale' 16.16: 0x10000 is the font's own units. 0, ERR_FULL from the
 * sink, ERR_BADFONT or ERR_UNSUPPORTED. */
int64_t font_glyph_points(const struct font *f, uint32_t glyph, int64_t scale, struct font_sink *sink);

/* A shaped glyph's flags while shape.c and layout.c have it, above
 * GLYPHF_*'s, and cleared before ShapeRun returns. */
enum : uint32_t {
    FONT_GLYPH_ZWNJ = 1u << 30,         /* U+200C: a ligature stops at it */
    FONT_GLYPH_FALLBACK = 1u << 31,     /* a space's width set by shape.c, not hmtx */
};

/* layout.c: GSUB's lookups for FONT_FEAT_* 'features' over the run's
 * glyphs, then (after hmtx's advances) GPOS's kerning or the kern
 * table's. Each 0, ERR_BADFONT, or ERR_UNSUPPORTED (too many lookups);
 * a lookup type that isn't built is skipped, with *warn's
 * FONT_WARN_STUB_SHAPER. A glyph above 0xffff is a control: it matches
 * nothing and stops a pair or ligature across it. GLYPHF_HIDDEN glyphs
 * are passed over, as if not there. */
int64_t font_substitute(const struct font *f, struct GlyphRun *run, uint32_t features, uint32_t *warn);
int64_t font_position(const struct font *f, struct GlyphRun *run, uint32_t features, uint32_t *warn);

/* stubs.c: §14.4's, each doing its fallback and giving its warning. */
uint32_t font_shape_complex_stub(const struct font *f, struct GlyphRun *run);
uint32_t font_bidi_reorder_stub(struct GlyphRun *run);
uint32_t font_apply_variations_stub(const struct font *f);
int64_t font_raster_color_stub(const struct font *f, uint32_t glyph, uint32_t size);
int64_t font_vertical_metrics_stub(const struct font *f, uint32_t glyph, struct GlyphMetrics *m);
void font_tt_hint_glyph_stub(const struct font *f, uint32_t glyph, uint32_t size);

#endif /* LIB_FONT_SFNT_H */
