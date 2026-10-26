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
    uint16_t upem, nglyphs, nhmetrics;
    bool loca_long;
};
static_assert(sizeof(struct font) <= sizeof(struct FontFace));

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

#endif /* LIB_FONT_SFNT_H */
