/*
 * lib/font/sfnt.c -- OpenFont and the tables it reads (V8 step 1)
 *
 * The file is read in place, every field through utility's checked
 * readers (Span), so a table that's short or an offset that's wrong is
 * ERR_BADFONT and never a read past the file. OpenFont checks what the
 * other calls rely on once: the tables there and long enough, head's
 * magic and units, the glyph count, hmtx's and loca's lengths, and a
 * cmap subtable whose arrays fit. After that a call can only find a
 * glyph's own data wrong, and says so.
 *
 * Written from ISO/IEC 14496-22 (§5.2's tables) and Apple's TrueType
 * Reference Manual (docs §14.1).
 */
#include "sfnt.h"

static uint16_t u16(struct Span s, uint64_t off)
{
    uint16_t v = 0;

    SpanReadU16BE(s, off, &v);
    return v;
}

static uint32_t u32(struct Span s, uint64_t off)
{
    uint32_t v = 0;

    SpanReadU32BE(s, off, &v);
    return v;
}

int64_t font_mulfix(int64_t a, int64_t b)
{
    bool neg = (a < 0) != (b < 0);
    int64_t c;

    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    c = (a * b + 0x8000) >> 16;
    return neg ? -c : c;
}

int64_t font_divfix(int64_t a, int64_t b)
{
    bool neg = (a < 0) != (b < 0);
    int64_t c;

    if (b == 0)
        return 0;
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    c = ((a << 16) + b / 2) / b;
    return neg ? -c : c;
}

/* The table 'tag' as a span of the file, or an empty one. */
static bool find(struct Span file, uint16_t ntables, uint32_t tag, struct Span *out)
{
    for (uint32_t i = 0; i < ntables; i++) {
        uint64_t rec = 12 + 16 * (uint64_t)i;

        if (u32(file, rec) == tag)
            return SpanSlice(file, u32(file, rec + 8), u32(file, rec + 12), out);
    }
    *out = (struct Span){ nullptr, 0 };
    return true;
}

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* The best Unicode subtable (§5.2.3's order: 3,10 then 3,1 then 0,*),
 * its arrays checked against its length. */
static int64_t pick_cmap(struct font *f, struct Span cmap)
{
    uint16_t n = u16(cmap, 2);
    int best = 0;

    if (cmap.sp_Len < 4 || 4 + 8 * (uint64_t)n > cmap.sp_Len)
        return ERR_BADFONT;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t plat = u16(cmap, 4 + 8 * i), enc = u16(cmap, 6 + 8 * i);
        uint32_t off = u32(cmap, 8 + 8 * i);
        uint16_t fmt = u16(cmap, off);
        int rank = 0;
        struct Span sub;

        if (off >= cmap.sp_Len)
            return ERR_BADFONT;
        if (fmt == 12 && (plat == 3 && enc == 10))
            rank = 4;
        else if (fmt == 12 && plat == 0)
            rank = 3;
        else if (fmt == 4 && plat == 3 && enc == 1)
            rank = 2;
        else if (fmt == 4 && plat == 0)
            rank = 1;
        if (rank <= best)
            continue;
        if (fmt == 4) {
            uint16_t len = u16(cmap, off + 2), seg2 = u16(cmap, off + 6);

            if (!SpanSlice(cmap, off, len, &sub) || seg2 == 0 || seg2 % 2 ||
                16 + 4 * (uint64_t)seg2 > len)
                return ERR_BADFONT;
        } else {
            uint32_t len = u32(cmap, off + 4), groups = u32(cmap, off + 12);

            if (!SpanSlice(cmap, off, len, &sub) || len < 16 || (len - 16) / 12 < groups)
                return ERR_BADFONT;
        }
        f->cmap = sub;
        f->cmap_format = fmt;
        best = rank;
    }
    return 0;
}

int64_t OpenFont(struct FontFace *face, struct Span file)
{
    struct font *f = (struct font *)face->ff_Private;
    uint32_t version = u32(file, 0);
    uint16_t ntables = u16(file, 4);
    struct Span cmap;
    int64_t e;

    *face = (struct FontFace){ 0 };
    f->file = file;
    if (file.sp_Len < 12)
        return ERR_BADFONT;
    if (version == TAG('O', 'T', 'T', 'O') || version == TAG('t', 't', 'c', 'f'))
        return ERR_UNSUPPORTED;
    if (version != 0x00010000 && version != TAG('t', 'r', 'u', 'e'))
        return ERR_BADFONT;
    if (12 + 16 * (uint64_t)ntables > file.sp_Len)
        return ERR_BADFONT;
    if (!find(file, ntables, TAG('h', 'e', 'a', 'd'), &f->head) ||
        !find(file, ntables, TAG('m', 'a', 'x', 'p'), &f->maxp) ||
        !find(file, ntables, TAG('h', 'h', 'e', 'a'), &f->hhea) ||
        !find(file, ntables, TAG('h', 'm', 't', 'x'), &f->hmtx) ||
        !find(file, ntables, TAG('l', 'o', 'c', 'a'), &f->loca) ||
        !find(file, ntables, TAG('g', 'l', 'y', 'f'), &f->glyf) ||
        !find(file, ntables, TAG('c', 'm', 'a', 'p'), &cmap) ||
        !find(file, ntables, TAG('O', 'S', '/', '2'), &f->os2) ||
        !find(file, ntables, TAG('p', 'o', 's', 't'), &f->post) ||
        !find(file, ntables, TAG('n', 'a', 'm', 'e'), &f->name) ||
        !find(file, ntables, TAG('G', 'S', 'U', 'B'), &f->gsub) ||
        !find(file, ntables, TAG('G', 'P', 'O', 'S'), &f->gpos) ||
        !find(file, ntables, TAG('G', 'D', 'E', 'F'), &f->gdef) ||
        !find(file, ntables, TAG('k', 'e', 'r', 'n'), &f->kern))
        return ERR_BADFONT;
    for (uint32_t i = 0; i < ntables; i++) {
        uint32_t tag = u32(file, 12 + 16 * (uint64_t)i);

        if (tag == TAG('f', 'v', 'a', 'r'))
            f->extras |= FONT_HAS_FVAR;
        if (tag == TAG('C', 'O', 'L', 'R') || tag == TAG('C', 'B', 'D', 'T') || tag == TAG('s', 'b', 'i', 'x') ||
            tag == TAG('S', 'V', 'G', ' '))
            f->extras |= FONT_HAS_COLOR;
    }
    if (!f->glyf.sp_Len) {
        find(file, ntables, TAG('C', 'F', 'F', ' '), &cmap);
        return cmap.sp_Len ? ERR_UNSUPPORTED : ERR_BADFONT;
    }
    if (f->head.sp_Len < 54 || u32(f->head, 12) != 0x5f0f3cf5 || f->maxp.sp_Len < 6 ||
        f->hhea.sp_Len < 36 || (f->post.sp_Len && f->post.sp_Len < 32))
        return ERR_BADFONT;
    f->upem = u16(f->head, 18);
    f->loca_long = u16(f->head, 50) == 1;
    f->nglyphs = u16(f->maxp, 4);
    f->nhmetrics = u16(f->hhea, 34);
    if (f->upem < 16 || f->upem > 16384 || u16(f->head, 50) > 1 || f->nglyphs == 0 ||
        f->nhmetrics == 0 || f->nhmetrics > f->nglyphs ||
        f->hmtx.sp_Len < 4 * (uint64_t)f->nhmetrics + 2 * (uint64_t)(f->nglyphs - f->nhmetrics) ||
        f->loca.sp_Len < ((uint64_t)f->nglyphs + 1) * (f->loca_long ? 4 : 2))
        return ERR_BADFONT;
    if (cmap.sp_Len && (e = pick_cmap(f, cmap)) != 0)
        return e;
    return 0;
}

void FontInfo(const struct FontFace *face, struct FontMetrics *m)
{
    const struct font *f = font_of(face);
    uint16_t os2v = u16(f->os2, 0);

    *m = (struct FontMetrics){
        .fm_UnitsPerEm = f->upem,
        .fm_NumGlyphs = f->nglyphs,
        .fm_Ascender = (int16_t)u16(f->hhea, 4),
        .fm_Descender = (int16_t)u16(f->hhea, 6),
        .fm_LineGap = (int16_t)u16(f->hhea, 8),
        .fm_XMin = (int16_t)u16(f->head, 36),
        .fm_YMin = (int16_t)u16(f->head, 38),
        .fm_XMax = (int16_t)u16(f->head, 40),
        .fm_YMax = (int16_t)u16(f->head, 42),
    };
    if (f->os2.sp_Len >= 78) {
        m->fm_Weight = u16(f->os2, 4);
        m->fm_FsSelection = u16(f->os2, 62);
        m->fm_TypoAscender = (int16_t)u16(f->os2, 68);
        m->fm_TypoDescender = (int16_t)u16(f->os2, 70);
        m->fm_TypoLineGap = (int16_t)u16(f->os2, 72);
        m->fm_WinAscent = u16(f->os2, 74);
        m->fm_WinDescent = u16(f->os2, 76);
    }
    if (os2v >= 2 && f->os2.sp_Len >= 90) {
        m->fm_XHeight = (int16_t)u16(f->os2, 86);
        m->fm_CapHeight = (int16_t)u16(f->os2, 88);
    }
    if (f->post.sp_Len) {
        m->fm_ItalicAngle = (int32_t)u32(f->post, 4);
        m->fm_UnderlinePosition = (int16_t)u16(f->post, 8);
        m->fm_UnderlineThickness = (int16_t)u16(f->post, 10);
        m->fm_FixedPitch = u32(f->post, 12);
    }
}

int64_t FontName(const struct FontFace *face, uint32_t nameId, char *buffer, uint64_t size)
{
    const struct font *f = font_of(face);
    struct Span name = f->name;
    uint16_t count = u16(name, 2), strings = u16(name, 4);
    int best = 0;
    uint64_t at = 0, len = 0;
    bool wide = false, found = false;

    for (uint32_t i = 0; i < count && 6 + 12 * (uint64_t)(i + 1) <= name.sp_Len; i++) {
        uint64_t r = 6 + 12 * (uint64_t)i;
        uint16_t plat = u16(name, r), enc = u16(name, r + 2), lang = u16(name, r + 4);
        int rank = 0;

        if (u16(name, r + 6) != nameId)
            continue;
        if (plat == 3 && enc == 1 && lang == 0x409)
            rank = 4;
        else if (plat == 3 && enc == 1)
            rank = 3;
        else if (plat == 0)
            rank = 2;
        else if (plat == 1 && enc == 0 && lang == 0)
            rank = 1;
        if (rank > best) {
            best = rank;
            len = u16(name, r + 8);
            at = (uint64_t)strings + u16(name, r + 10);
            wide = plat != 1;
            found = true;
        }
    }
    if (!found)
        return ERR_NOTFOUND;
    struct Span str;
    uint64_t n = 0;

    if (!SpanSlice(name, at, len, &str))
        return ERR_BADFONT;
    for (uint64_t i = 0; i < len; i += wide ? 2 : 1) {
        uint32_t c = wide ? u16(str, i) : str.sp_Data[i];

        if (wide && i + 1 >= len)
            break;
        if (size && n + 1 < size)
            buffer[n] = c >= 0x20 && c < 0x7f ? (char)c : '?';
        n++;
    }
    if (size)
        buffer[n < size ? n : size - 1] = 0;
    return (int64_t)n;
}

uint32_t MapCodepoint(const struct FontFace *face, uint32_t c)
{
    const struct font *f = font_of(face);
    struct Span s = f->cmap;
    uint32_t g = 0;

    if (f->cmap_format == 4) {
        uint32_t segs = u16(s, 6) / 2, lo = 0, hi = segs;
        uint64_t ends = 14, starts = 16 + 2 * (uint64_t)segs;
        uint64_t deltas = starts + 2 * (uint64_t)segs, ranges = deltas + 2 * (uint64_t)segs;

        if (c > 0xffff)
            return 0;
        while (lo < hi) {               /* the first segment ending at or after c */
            uint32_t mid = (lo + hi) / 2;

            if (u16(s, ends + 2 * mid) < c)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == segs || u16(s, starts + 2 * lo) > c)
            return 0;
        uint16_t delta = u16(s, deltas + 2 * lo), range = u16(s, ranges + 2 * lo);

        if (range == 0) {
            g = (c + delta) & 0xffff;
        } else {
            uint16_t v = 0;

            if (!SpanReadU16BE(s, ranges + 2 * lo + range + 2 * (c - u16(s, starts + 2 * lo)), &v))
                return 0;
            g = v ? (v + delta) & 0xffff : 0;
        }
    } else if (f->cmap_format == 12) {
        uint32_t lo = 0, hi = u32(s, 12);

        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint64_t grp = 16 + 12 * (uint64_t)mid;

            if (u32(s, grp + 4) < c) {
                lo = mid + 1;
            } else if (u32(s, grp) > c) {
                hi = mid;
            } else {
                g = u32(s, grp + 8) + (c - u32(s, grp));
                break;
            }
        }
    }
    return g < f->nglyphs ? g : 0;
}

int64_t GetGlyphMetrics(const struct FontFace *face, uint32_t glyph, struct GlyphMetrics *m)
{
    const struct font *f = font_of(face);
    uint32_t last = f->nhmetrics - 1u;

    if (glyph >= f->nglyphs)
        return ERR_BADARG;
    m->gm_Advance = u16(f->hmtx, 4 * (uint64_t)(glyph < last ? glyph : last));
    m->gm_LeftBearing = (int16_t)(glyph < f->nhmetrics
                                      ? u16(f->hmtx, 4 * (uint64_t)glyph + 2)
                                      : u16(f->hmtx, 4 * (uint64_t)f->nhmetrics + 2 * (uint64_t)(glyph - f->nhmetrics)));
    return 0;
}

int32_t ScaleUnits(const struct FontFace *face, int32_t units, uint32_t size)
{
    return (int32_t)font_mulfix(units, font_divfix(size, font_of(face)->upem));
}
