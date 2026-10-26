/*
 * lib/font/shape.c -- ShapeRun, FitLine and LineSpacing (V8 step 2)
 *
 * §14.3.3's pipeline, into the caller's GlyphRun and allocating nothing:
 *
 *   1. UTF-8 decoded, each ill-formed part of it U+FFFD (Unicode §3.9's
 *      "maximal subpart"), each character's first byte its cluster.
 *   2. Characters outside Latin noted: a complex script or a right-to-
 *      left one hands the run to §14.4's stubs (stubs.c), one glyph a
 *      character with hmtx's advances, and a warning.
 *   3. cmap. A space the font hasn't got is its space glyph at the
 *      width Unicode gives it (an em over n, a figure's, a period's), a
 *      no-break hyphen U+2010's glyph. Controls (tab, the newlines) are
 *      glyphs that match nothing, and default ignorables (the soft
 *      hyphen, ZWSP, the joiners) glyphs lookups pass over.
 *   4. GSUB's liga, tnum and pnum, hmtx's advances, then GPOS's kern or
 *      the kern table (layout.c).
 *   5. Controls and ignorables made the space glyph, zero wide: there
 *      for FitLine to find, and not drawn (GLYPHF_HIDDEN).
 *
 * FitLine is stage 5 of §14.3.3: a line broken at spaces, tabs,
 * newlines, hyphens and soft hyphens, greedily; LineSpacing its height.
 */
#include "sfnt.h"

constexpr uint32_t CONTROL = 0x10000;       /* layout.c's: a glyph that matches nothing */
constexpr int32_t TAB_SPACES = 8;           /* a tab stop every eight spaces */

/* The character at text[*at], *at moved past it. */
static uint32_t decode(const uint8_t *t, uint64_t len, uint64_t *at)
{
    uint8_t b = t[(*at)++], lo = 0x80, hi = 0xbf;
    uint32_t c, need;

    if (b < 0x80)
        return b;
    if (b >= 0xc2 && b <= 0xdf) {
        need = 1, c = b & 0x1f;
    } else if (b >= 0xe0 && b <= 0xef) {
        need = 2, c = b & 0x0f;
        lo = b == 0xe0 ? 0xa0 : 0x80;   /* not overlong */
        hi = b == 0xed ? 0x9f : 0xbf;   /* not a surrogate */
    } else if (b >= 0xf0 && b <= 0xf4) {
        need = 3, c = b & 0x07;
        lo = b == 0xf0 ? 0x90 : 0x80;
        hi = b == 0xf4 ? 0x8f : 0xbf;   /* not past U+10FFFF */
    } else {
        return 0xfffd;
    }
    while (need--) {
        if (*at >= len || t[*at] < lo || t[*at] > hi)
            return 0xfffd;              /* the subpart so far, one U+FFFD */
        c = c << 6 | (t[(*at)++] & 0x3f);
        lo = 0x80, hi = 0xbf;
    }
    return c;
}

/* Where §14.4's stubs take over: scripts that need more than Latin's
 * shaping (marks placed, clusters reordered), and those written right
 * to left. Hangul's syllables, precomposed, need neither. */
static const struct {
    uint32_t lo, hi, warn;
} stubbed[] = {
    { 0x0300, 0x036f, FONT_WARN_STUB_SHAPER },  /* combining marks */
    { 0x0483, 0x0489, FONT_WARN_STUB_SHAPER },
    { 0x0590, 0x08ff, FONT_WARN_STUB_SHAPER | FONT_WARN_STUB_BIDI },  /* Hebrew to Arabic */
    { 0x0900, 0x109f, FONT_WARN_STUB_SHAPER },  /* Indic, Thai to Myanmar */
    { 0x1100, 0x11ff, FONT_WARN_STUB_SHAPER },  /* Hangul's jamo */
    { 0x1700, 0x18af, FONT_WARN_STUB_SHAPER },  /* Philippine, Khmer, Mongolian */
    { 0x1900, 0x1cff, FONT_WARN_STUB_SHAPER },
    { 0x1dc0, 0x1dff, FONT_WARN_STUB_SHAPER },
    { 0x200f, 0x200f, FONT_WARN_STUB_BIDI },     /* RLM */
    { 0x202a, 0x202e, FONT_WARN_STUB_BIDI },     /* embeddings and overrides */
    { 0x2066, 0x2069, FONT_WARN_STUB_BIDI },     /* isolates */
    { 0x20d0, 0x20ff, FONT_WARN_STUB_SHAPER },
    { 0xa800, 0xabff, FONT_WARN_STUB_SHAPER },
    { 0xd7b0, 0xd7ff, FONT_WARN_STUB_SHAPER },
    { 0xfb1d, 0xfdff, FONT_WARN_STUB_SHAPER | FONT_WARN_STUB_BIDI },
    { 0xfe20, 0xfe2f, FONT_WARN_STUB_SHAPER },
    { 0xfe70, 0xfefe, FONT_WARN_STUB_SHAPER | FONT_WARN_STUB_BIDI },
    { 0x10800, 0x10fff, FONT_WARN_STUB_SHAPER | FONT_WARN_STUB_BIDI },
    { 0x11000, 0x11fff, FONT_WARN_STUB_SHAPER },
    { 0x1e800, 0x1efff, FONT_WARN_STUB_SHAPER | FONT_WARN_STUB_BIDI },
};

/* Default ignorables (Unicode's DerivedCoreProperties): not drawn, and
 * passed over by lookups, so a soft hyphen doesn't stop a kern. */
static const struct {
    uint32_t lo, hi;
} ignorable[] = {
    { 0x00ad, 0x00ad }, { 0x034f, 0x034f }, { 0x061c, 0x061c }, { 0x115f, 0x1160 }, { 0x17b4, 0x17b5 },
    { 0x180b, 0x180f }, { 0x200b, 0x200f }, { 0x202a, 0x202e }, { 0x2060, 0x206f }, { 0x3164, 0x3164 },
    { 0xfe00, 0xfe0f }, { 0xfeff, 0xfeff }, { 0xffa0, 0xffa0 }, { 0xfff0, 0xfff8 }, { 0x1bca0, 0x1bca3 },
    { 0x1d173, 0x1d17a }, { 0xe0000, 0xe0fff },
};

static uint32_t warning_for(uint32_t c)
{
    if (c < 0x0300)
        return 0;
    for (unsigned i = 0; i < sizeof stubbed / sizeof stubbed[0]; i++)
        if (c >= stubbed[i].lo && c <= stubbed[i].hi)
            return stubbed[i].warn;
    return 0;
}

static bool is_ignorable(uint32_t c)
{
    for (unsigned i = 0; i < sizeof ignorable / sizeof ignorable[0]; i++)
        if (c >= ignorable[i].lo && c <= ignorable[i].hi)
            return true;
    return false;
}

/* What a character is to the line breaker and the drawer. */
static uint32_t flags_for(uint32_t c)
{
    switch (c) {
    case 0x09: return GLYPHF_TAB | GLYPHF_HIDDEN;
    case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x85: case 0x2028: case 0x2029:
        return GLYPHF_NEWLINE | GLYPHF_HIDDEN;
    case 0x20: case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006:
    case 0x2008: case 0x2009: case 0x200a: case 0x205f: case 0x3000:
        return GLYPHF_SPACE;
    case 0x200b: return GLYPHF_SPACE | GLYPHF_HIDDEN;      /* ZWSP: a break, unseen */
    case 0x2d: case 0x2010: return GLYPHF_HYPHEN;
    case 0xad: return GLYPHF_SOFT_HYPHEN | GLYPHF_HIDDEN;
    }
    if (c < 0x20 || (c >= 0x7f && c < 0xa0))
        return GLYPHF_HIDDEN;
    return is_ignorable(c) ? GLYPHF_HIDDEN : 0;
}

static uint32_t advance_of(const struct FontFace *face, uint32_t glyph)
{
    struct GlyphMetrics gm = { 0, 0 };

    GetGlyphMetrics(face, glyph, &gm);
    return gm.gm_Advance;
}

/* A space the font hasn't a glyph for: its width, in the font's units,
 * if Unicode gives it one (the em over n, rounded), or -1. */
static int32_t space_width(const struct FontFace *face, uint32_t c, uint32_t space)
{
    int32_t upem = font_of(face)->upem, n = 0;
    uint32_t g;

    switch (c) {
    case 0x00a0: return (int32_t)advance_of(face, space);
    case 0x2001: case 0x2003: case 0x3000: n = 1; break;
    case 0x2000: case 0x2002: n = 2; break;
    case 0x2004: n = 3; break;
    case 0x2005: n = 4; break;
    case 0x2009: n = 5; break;
    case 0x2006: n = 6; break;
    case 0x200a: n = 16; break;
    case 0x202f: return (int32_t)advance_of(face, space) / 2;
    case 0x205f: return upem * 4 / 18;
    case 0x2007:                        /* a figure's */
        for (uint32_t d = '0'; d <= '9'; d++)
            if ((g = MapCodepoint(face, d)) != 0)
                return (int32_t)advance_of(face, g);
        return (int32_t)advance_of(face, space);
    case 0x2008:                        /* a period's */
        if ((g = MapCodepoint(face, '.')) != 0 || (g = MapCodepoint(face, ',')) != 0)
            return (int32_t)advance_of(face, g);
        return (int32_t)advance_of(face, space);
    default: return -1;
    }
    return (upem + n / 2) / n;
}

int64_t ShapeRun(const struct FontFace *face, const char *text, uint64_t len, uint32_t features,
                 struct GlyphRun *run)
{
    const struct font *f = font_of(face);
    const uint8_t *t = (const uint8_t *)text;
    struct ShapedGlyph *gs = run->gr_Glyphs;
    uint32_t space = MapCodepoint(face, ' '), warn = 0, n = 0;
    int64_t e = 0;

    run->gr_Count = 0;
    run->gr_Advance = 0;
    run->gr_Warnings = 0;
    if (len >= 1ull << 31)
        return ERR_BADARG;
    for (uint64_t at = 0; at < len;) {
        uint32_t cluster = (uint32_t)at, c = decode(t, len, &at), flags = flags_for(c);
        uint32_t g = MapCodepoint(face, c);
        int32_t width = 0;

        if (n == run->gr_Max)
            return ERR_FULL;
        warn |= warning_for(c);
        if (c == 0x0a && n && gs[n - 1].sg_Flags & GLYPHF_NEWLINE && t[gs[n - 1].sg_Cluster] == 0x0d)
            gs[n - 1].sg_Flags &= ~GLYPHF_NEWLINE;  /* CR LF is one newline: LF's */
        if (flags & (GLYPHF_TAB | GLYPHF_NEWLINE) || (flags & GLYPHF_HIDDEN && c < 0xa0)) {
            g = CONTROL;
        } else if (g == 0 && space && (width = space_width(face, c, space)) >= 0) {
            g = space;
            flags |= FONT_GLYPH_FALLBACK;
        } else if (g == 0 && c == 0x2011) {
            g = MapCodepoint(face, 0x2010);
        }
        if (c == 0x200c)
            flags |= FONT_GLYPH_ZWNJ;
        gs[n++] = (struct ShapedGlyph){ g, cluster, width, 0, 0, flags };
    }
    run->gr_Count = n;

    if (warn & FONT_WARN_STUB_BIDI)
        warn |= font_bidi_reorder_stub(run);
    if (f->extras & FONT_HAS_FVAR)
        warn |= font_apply_variations_stub(f);
    if (f->extras & FONT_HAS_COLOR && font_raster_color_stub(f, 0, 0) == ERR_UNSUPPORTED)
        warn |= FONT_WARN_STUB_COLOR;
    /* A script the stub has: no lookups at all. A lookup type that isn't
     * built warns the same, but only that lookup is passed over. */
    bool complex = warn & FONT_WARN_STUB_SHAPER;

    if (complex)
        warn |= font_shape_complex_stub(f, run);
    else
        e = font_substitute(f, run, features, &warn);
    for (uint32_t i = 0; i < run->gr_Count && e == 0; i++)
        if (!(gs[i].sg_Flags & FONT_GLYPH_FALLBACK))
            gs[i].sg_Advance = gs[i].sg_Glyph < CONTROL ? (int32_t)advance_of(face, gs[i].sg_Glyph) : 0;
    if (e == 0 && !complex)
        e = font_position(f, run, features, &warn);
    if (e != 0) {
        run->gr_Count = 0;
        return e;
    }
    for (uint32_t i = 0; i < run->gr_Count; i++) {
        struct ShapedGlyph *sg = &gs[i];

        if (sg->sg_Flags & GLYPHF_HIDDEN || sg->sg_Glyph >= CONTROL) {
            sg->sg_Glyph = space;
            sg->sg_Advance = sg->sg_XOffset = sg->sg_YOffset = 0;
        }
        sg->sg_Flags &= ~(FONT_GLYPH_FALLBACK | FONT_GLYPH_ZWNJ);
        run->gr_Advance += sg->sg_Advance;
    }
    run->gr_Warnings = warn;
    return 0;
}

void FitLine(const struct FontFace *face, struct GlyphRun *run, uint32_t first, int32_t width, uint32_t size,
             struct LineFit *fit)
{
    struct ShapedGlyph *gs = run->gr_Glyphs;
    uint32_t space = MapCodepoint(face, ' '), hyphen = MapCodepoint(face, '-');
    int32_t stop = TAB_SPACES * (int32_t)advance_of(face, space), hyphen_w = (int32_t)advance_of(face, hyphen);
    int32_t x = 0, ink = 0;             /* the pen, and where the last glyph not a space ended */
    struct LineFit best = { 0, 0, 0, hyphen };

    if (stop <= 0)
        stop = font_of(face)->upem;
    for (uint32_t i = first; i < run->gr_Count; i++) {
        struct ShapedGlyph *sg = &gs[i];
        uint32_t taken = i - first + 1;

        if (sg->sg_Flags & GLYPHF_NEWLINE) {
            *fit = (struct LineFit){ taken, LINEF_NEWLINE | (i + 1 == run->gr_Count ? LINEF_END : 0),
                                     ScaleUnits(face, ink, size), hyphen };
            return;
        }
        if (sg->sg_Flags & GLYPHF_TAB) {
            int32_t adv = stop - x % stop;

            run->gr_Advance += adv - sg->sg_Advance;
            sg->sg_Advance = adv;
        }
        if (sg->sg_Flags & (GLYPHF_SPACE | GLYPHF_TAB)) {   /* spaces hang past the edge */
            x += sg->sg_Advance;
            best = (struct LineFit){ taken, 0, ScaleUnits(face, ink, size), hyphen };
            continue;
        }
        if (i > first && sg->sg_Advance > 0 && ScaleUnits(face, x + sg->sg_Advance, size) > width) {
            if (best.lf_Count) {
                *fit = best;
            } else {
                *fit = (struct LineFit){ i - first, LINEF_FORCED, ScaleUnits(face, ink, size), hyphen };
            }
            return;
        }
        x += sg->sg_Advance;
        if (sg->sg_Advance || !(sg->sg_Flags & GLYPHF_HIDDEN))
            ink = x;
        if (sg->sg_Flags & GLYPHF_HYPHEN)
            best = (struct LineFit){ taken, 0, ScaleUnits(face, ink, size), hyphen };
        if (sg->sg_Flags & GLYPHF_SOFT_HYPHEN && hyphen && ScaleUnits(face, ink + hyphen_w, size) <= width)
            best = (struct LineFit){ taken, LINEF_HYPHEN, ScaleUnits(face, ink + hyphen_w, size), hyphen };
    }
    *fit = (struct LineFit){ first < run->gr_Count ? run->gr_Count - first : 0, LINEF_END,
                             ScaleUnits(face, ink, size), hyphen };
}

void LineSpacing(const struct FontFace *face, struct LineSpacing *ls)
{
    struct FontMetrics m;
    bool typo;

    FontInfo(face, &m);
    typo = m.fm_FsSelection & 0x80;     /* USE_TYPO_METRICS */
    ls->ls_Ascent = typo && m.fm_TypoAscender ? m.fm_TypoAscender : m.fm_Ascender;
    ls->ls_Descent = -(typo && m.fm_TypoDescender ? m.fm_TypoDescender : m.fm_Descender);
    ls->ls_LineGap = typo && m.fm_TypoLineGap ? m.fm_TypoLineGap : m.fm_LineGap;
    ls->ls_Height = ls->ls_Ascent + ls->ls_Descent + ls->ls_LineGap;
}
