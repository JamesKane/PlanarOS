/*
 * lib/font/layout.c -- GDEF, GSUB, GPOS and kern: the font's own layout (V8 step 2)
 *
 * The OpenType layout §14.3.2 builds, read in place: GSUB's single
 * substitutions (tnum, pnum) and ligatures (liga), GPOS's pair
 * adjustments (kern), each perhaps behind an extension lookup, and the
 * old kern table's format 0 where GPOS has no kerning. GDEF's classes
 * say which glyphs a lookup passes over (its IgnoreMarks and the rest).
 *
 * A table is found for the script 'latn' (else 'DFLT', else 'dflt'),
 * and the language 'ENG ' (else the script's default). The lookups of
 * the features asked for are gathered, sorted and run in that order,
 * each over the whole run before the next. Every read is checked: a
 * table that points outside itself, a coverage index past its array, a
 * substitute past the last glyph, is ERR_BADFONT. A lookup type this
 * doesn't build (contextual, marks, cursive) is skipped, and the run
 * warned (§14.4's shaper stub).
 *
 * Written from ISO/IEC 14496-22 §6.2 (common tables, GDEF, GSUB, GPOS)
 * and §5.6.1 (kern), and checked against HarfBuzz's results by
 * tools/font-oracle's pass 2, never its code (docs §14.1).
 */
#include "sfnt.h"

constexpr unsigned MAX_LOOKUPS = 256;       /* a run's, per table */
constexpr unsigned MAX_COMPONENTS = 64;     /* a ligature's */
constexpr uint32_t CONTROL = 0x10000;       /* above any glyph: shape.c's controls */

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* Lookup flags */
enum : uint16_t {
    IGNORE_BASE = 1u << 1,
    IGNORE_LIGATURES = 1u << 2,
    IGNORE_MARKS = 1u << 3,
    USE_MARK_SET = 1u << 4,
    MARK_ATTACH_TYPE = 0xff00,
};

/* GDEF's glyph classes */
enum : uint32_t {
    CLASS_BASE = 1,
    CLASS_LIGATURE = 2,
    CLASS_MARK = 3,
};

/* A pass over a table: the run, the lookup's flags, and whether
 * anything read was outside where it should be. */
struct lay {
    const struct font *f;
    struct GlyphRun *run;
    uint16_t flags;
    struct Span mark_set;               /* USE_MARK_SET's coverage */
    bool gsub;
    bool bad;
};

static uint16_t g16(struct lay *l, struct Span s, uint64_t off)
{
    uint16_t v = 0;

    if (!SpanReadU16BE(s, off, &v))
        l->bad = true;
    return v;
}

static uint32_t g32(struct lay *l, struct Span s, uint64_t off)
{
    uint32_t v = 0;

    if (!SpanReadU32BE(s, off, &v))
        l->bad = true;
    return v;
}

static bool bad(struct lay *l)
{
    l->bad = true;
    return false;
}

/* Where an offset in 's' points: from there to s's end. */
static struct Span from(struct lay *l, struct Span s, uint64_t off)
{
    struct Span o = { nullptr, 0 };

    if (off > s.sp_Len || !SpanSlice(s, off, s.sp_Len - off, &o))
        l->bad = true;
    return o;
}

/* An array of 'n' records of 'size' bytes at 'off': there, or bad. */
static bool fits(struct lay *l, struct Span s, uint64_t off, uint64_t n, uint64_t size)
{
    if (off + n * size > s.sp_Len)
        l->bad = true;
    return !l->bad;
}

/* The glyph's index in a coverage table, or -1. */
static int64_t coverage(struct lay *l, struct Span c, uint32_t g)
{
    uint16_t fmt = g16(l, c, 0), n = g16(l, c, 2);
    uint32_t lo = 0, hi = n;

    if (g >= CONTROL)
        return -1;
    if (fmt == 1) {
        if (!fits(l, c, 4, n, 2))
            return -1;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, at = g16(l, c, 4 + 2 * (uint64_t)mid);

            if (at == g)
                return mid;
            if (at < g)
                lo = mid + 1;
            else
                hi = mid;
        }
        return -1;
    }
    if (fmt != 2 || !fits(l, c, 4, n, 6)) {
        l->bad = true;
        return -1;
    }
    while (lo < hi) {                   /* the first range ending at or after g */
        uint32_t mid = (lo + hi) / 2;

        if (g16(l, c, 4 + 6 * (uint64_t)mid + 2) < g)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == n || g16(l, c, 4 + 6 * (uint64_t)lo) > g)
        return -1;
    return g16(l, c, 4 + 6 * (uint64_t)lo + 4) + (int64_t)(g - g16(l, c, 4 + 6 * (uint64_t)lo));
}

/* The glyph's class in a class definition table: 0 if it isn't in it. */
static uint32_t class_of(struct lay *l, struct Span c, uint32_t g)
{
    uint16_t fmt = g16(l, c, 0);

    if (g >= CONTROL)
        return 0;
    if (fmt == 1) {
        uint16_t start = g16(l, c, 2), n = g16(l, c, 4);

        if (!fits(l, c, 6, n, 2) || g < start || g - start >= n)
            return 0;
        return g16(l, c, 6 + 2 * (uint64_t)(g - start));
    }
    uint16_t n = g16(l, c, 2);
    uint32_t lo = 0, hi = n;

    if (fmt != 2 || !fits(l, c, 4, n, 6)) {
        l->bad = true;
        return 0;
    }
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;

        if (g16(l, c, 4 + 6 * (uint64_t)mid + 2) < g)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == n || g16(l, c, 4 + 6 * (uint64_t)lo) > g)
        return 0;
    return g16(l, c, 4 + 6 * (uint64_t)lo + 4);
}

/* GDEF's class for the glyph; without its class table, every glyph's
 * a base, as a font with no GDEF has only bases. */
static uint32_t glyph_class(struct lay *l, uint32_t g)
{
    struct Span gdef = l->f->gdef;
    uint16_t off = gdef.sp_Len ? g16(l, gdef, 4) : 0;

    return off ? class_of(l, from(l, gdef, off), g) : CLASS_BASE;
}

/* Is glyph i one the lookup at hand passes over, by its flags and
 * GDEF's class? As the glyph a lookup starts at, that's all. */
static bool ignored(struct lay *l, uint32_t i)
{
    const struct ShapedGlyph *sg = &l->run->gr_Glyphs[i];
    uint32_t cls;

    if (sg->sg_Glyph >= CONTROL ||
        !(l->flags & (IGNORE_BASE | IGNORE_LIGATURES | IGNORE_MARKS | USE_MARK_SET | MARK_ATTACH_TYPE)))
        return false;
    cls = glyph_class(l, sg->sg_Glyph);
    if (cls == CLASS_BASE)
        return l->flags & IGNORE_BASE;
    if (cls == CLASS_LIGATURE)
        return l->flags & IGNORE_LIGATURES;
    if (cls != CLASS_MARK)
        return false;
    if (l->flags & IGNORE_MARKS)
        return true;
    if (l->flags & USE_MARK_SET)
        return coverage(l, l->mark_set, sg->sg_Glyph) < 0;
    if (l->flags & MARK_ATTACH_TYPE) {
        uint16_t off = g16(l, l->f->gdef, 10);

        return !off || class_of(l, from(l, l->f->gdef, off), sg->sg_Glyph) != (uint32_t)(l->flags >> 8);
    }
    return false;
}

/* And as a glyph after it, to be matched or passed: a default ignorable
 * is passed over too, but for ZWNJ in GSUB, which stops a ligature; a
 * control never is, and matches nothing. */
static bool skipped(struct lay *l, uint32_t i)
{
    uint32_t flags = l->run->gr_Glyphs[i].sg_Flags;

    if (l->run->gr_Glyphs[i].sg_Glyph >= CONTROL)
        return false;
    if (flags & GLYPHF_HIDDEN)
        return !(l->gsub && flags & FONT_GLYPH_ZWNJ);
    return ignored(l, i);
}

/* The next glyph after i the lookup doesn't pass over, or -1. */
static int64_t next_glyph(struct lay *l, uint32_t i)
{
    for (uint32_t j = i + 1; j < l->run->gr_Count; j++)
        if (!skipped(l, j))
            return j;
    return -1;
}

/* --- Scripts, features and lookups --- */

static struct Span find_tagged(struct lay *l, struct Span list, uint32_t tag, struct Span base)
{
    uint16_t n = g16(l, list, 0);

    if (!fits(l, list, 2, n, 6))
        return (struct Span){ nullptr, 0 };
    for (uint32_t i = 0; i < n; i++)
        if (g32(l, list, 2 + 6 * (uint64_t)i) == tag)
            return from(l, base, g16(l, list, 2 + 6 * (uint64_t)i + 4));
    return (struct Span){ nullptr, 0 };
}

/* Does the table's feature list have the tag, for any script? */
static bool has_feature(struct lay *l, struct Span table, uint32_t tag)
{
    struct Span list = from(l, table, g16(l, table, 6));
    uint16_t n = g16(l, list, 0);

    if (!fits(l, list, 2, n, 6))
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (g32(l, list, 2 + 6 * (uint64_t)i) == tag)
            return true;
    return false;
}

/* Feature 'index''s lookups added to the sorted set 'out' (*n of them). */
static int64_t add_lookups(struct lay *l, struct Span table, uint32_t index, uint16_t *out, uint32_t *n)
{
    struct Span list = from(l, table, g16(l, table, 6));
    uint16_t nf = g16(l, list, 0);

    if (index >= nf || !fits(l, list, 2, nf, 6))
        return ERR_BADFONT;
    struct Span feat = from(l, list, g16(l, list, 2 + 6 * (uint64_t)index + 4));
    uint16_t nl = g16(l, feat, 2);

    if (!fits(l, feat, 4, nl, 2))
        return ERR_BADFONT;
    for (uint32_t i = 0; i < nl; i++) {
        uint16_t lk = g16(l, feat, 4 + 2 * (uint64_t)i);
        uint32_t at = 0;

        while (at < *n && out[at] < lk)
            at++;
        if (at < *n && out[at] == lk)
            continue;
        if (*n == MAX_LOOKUPS)
            return ERR_UNSUPPORTED;
        for (uint32_t k = *n; k > at; k--)
            out[k] = out[k - 1];
        out[at] = lk;
        (*n)++;
    }
    return 0;
}

/* The lookups, in order, of the features 'tags' (and the language's
 * required one) for Latin in English. */
static int64_t gather(struct lay *l, struct Span table, const uint32_t *tags, unsigned ntags, uint16_t *out,
                      uint32_t *n)
{
    struct Span scripts, script, langsys, features;
    uint16_t nidx, required;
    int64_t e;

    *n = 0;
    if (table.sp_Len < 10 || g16(l, table, 0) != 1)
        return table.sp_Len ? ERR_BADFONT : 0;
    scripts = from(l, table, g16(l, table, 4));
    script = find_tagged(l, scripts, TAG('l', 'a', 't', 'n'), scripts);
    if (!script.sp_Len)
        script = find_tagged(l, scripts, TAG('D', 'F', 'L', 'T'), scripts);
    if (!script.sp_Len)
        script = find_tagged(l, scripts, TAG('d', 'f', 'l', 't'), scripts);
    if (l->bad)
        return ERR_BADFONT;
    if (!script.sp_Len)
        return 0;
    if (!fits(l, script, 2, 1, 2))
        return ERR_BADFONT;
    langsys = find_tagged(l, from(l, script, 2), TAG('E', 'N', 'G', ' '), script);
    if (!langsys.sp_Len && g16(l, script, 0))
        langsys = from(l, script, g16(l, script, 0));
    if (l->bad)
        return ERR_BADFONT;
    if (!langsys.sp_Len)
        return 0;
    required = g16(l, langsys, 2);
    nidx = g16(l, langsys, 4);
    if (!fits(l, langsys, 6, nidx, 2))
        return ERR_BADFONT;
    if (required != 0xffff && (e = add_lookups(l, table, required, out, n)) != 0)
        return e;
    features = from(l, table, g16(l, table, 6));
    for (uint32_t i = 0; i < nidx; i++) {
        uint16_t index = g16(l, langsys, 6 + 2 * (uint64_t)i);

        if (index >= g16(l, features, 0) || !fits(l, features, 2, index + 1u, 6))
            return ERR_BADFONT;
        for (unsigned t = 0; t < ntags; t++)
            if (g32(l, features, 2 + 6 * (uint64_t)index) == tags[t] &&
                (e = add_lookups(l, table, index, out, n)) != 0)
                return e;
    }
    return l->bad ? ERR_BADFONT : 0;
}

/* Lookup 'index' of the table: its type, its flags set in *l, and its
 * subtables' count. An extension's subtables are read through it by
 * subtable(). */
static struct Span lookup(struct lay *l, struct Span table, uint32_t index, uint16_t *type, uint16_t *nsub)
{
    struct Span list = from(l, table, g16(l, table, 8)), lk;
    uint16_t n = g16(l, list, 0);

    if (index >= n || !fits(l, list, 2, n, 2)) {
        l->bad = true;
        return (struct Span){ nullptr, 0 };
    }
    lk = from(l, list, g16(l, list, 2 + 2 * (uint64_t)index));
    *type = g16(l, lk, 0);
    l->flags = g16(l, lk, 2);
    *nsub = g16(l, lk, 4);
    fits(l, lk, 6, *nsub, 2);
    l->mark_set = (struct Span){ nullptr, 0 };
    if (l->flags & USE_MARK_SET) {
        uint16_t set = g16(l, lk, 6 + 2 * (uint64_t)*nsub);
        struct Span gdef = l->f->gdef;
        uint16_t off = gdef.sp_Len >= 14 && g32(l, gdef, 0) >= 0x00010002 ? g16(l, gdef, 12) : 0;

        if (off) {
            struct Span sets = from(l, gdef, off);

            if (set < g16(l, sets, 2) && fits(l, sets, 4, set + 1u, 4))
                l->mark_set = from(l, sets, g32(l, sets, 4 + 4 * (uint64_t)set));
        }
    }
    return lk;
}

/* Subtable k of a lookup of 'type', through an extension ('ext', 7 or
 * 9) if it's one: its span, and its real type in *real. */
static struct Span subtable(struct lay *l, struct Span lk, uint16_t type, uint16_t ext, uint32_t k, uint16_t *real)
{
    struct Span s = from(l, lk, g16(l, lk, 6 + 2 * (uint64_t)k));

    *real = type;
    if (type != ext)
        return s;
    if (g16(l, s, 0) != 1)
        l->bad = true;
    *real = g16(l, s, 2);
    if (*real == ext)                   /* an extension of an extension: a loop, perhaps */
        l->bad = true;
    return from(l, s, g32(l, s, 4));
}

/* --- GSUB --- */

/* A single substitution at glyph i. */
static bool single(struct lay *l, struct Span s, uint32_t i)
{
    struct ShapedGlyph *sg = &l->run->gr_Glyphs[i];
    uint16_t fmt = g16(l, s, 0);
    int64_t c = coverage(l, from(l, s, g16(l, s, 2)), sg->sg_Glyph);
    uint32_t g;

    if (c < 0)
        return false;
    if (fmt == 1) {
        g = (sg->sg_Glyph + g16(l, s, 4)) & 0xffff;
    } else if (fmt == 2) {
        if (c >= g16(l, s, 4) || !fits(l, s, 6, (uint64_t)c + 1, 2))
            return bad(l);
        g = g16(l, s, 6 + 2 * (uint64_t)c);
    } else {
        return bad(l);
    }
    if (g >= l->f->nglyphs)
        return bad(l);
    sg->sg_Glyph = g;
    return true;
}

/* A ligature starting at glyph i: the first in its set whose
 * components follow, the glyphs passed over between them kept after it. */
static bool ligature(struct lay *l, struct Span s, uint32_t i)
{
    struct GlyphRun *run = l->run;
    struct ShapedGlyph *gs = run->gr_Glyphs;
    int64_t c = coverage(l, from(l, s, g16(l, s, 2)), gs[i].sg_Glyph);
    struct Span set;
    uint16_t nlig;

    if (c < 0)
        return false;
    if (g16(l, s, 0) != 1 || c >= g16(l, s, 4) || !fits(l, s, 6, (uint64_t)c + 1, 2))
        return bad(l);
    set = from(l, s, g16(l, s, 6 + 2 * (uint64_t)c));
    nlig = g16(l, set, 0);
    if (!fits(l, set, 2, nlig, 2))
        return false;
    for (uint32_t k = 0; k < nlig; k++) {
        struct Span lig = from(l, set, g16(l, set, 2 + 2 * (uint64_t)k));
        uint16_t glyph = g16(l, lig, 0), ncomp = g16(l, lig, 2);
        uint32_t at[MAX_COMPONENTS];
        uint32_t m = 1, flags = gs[i].sg_Flags;

        if (l->bad || ncomp == 0 || ncomp > MAX_COMPONENTS || !fits(l, lig, 4, ncomp - 1u, 2))
            return bad(l);
        at[0] = i;
        for (; m < ncomp; m++) {
            int64_t j = next_glyph(l, at[m - 1]);

            if (j < 0 || gs[j].sg_Glyph != g16(l, lig, 4 + 2 * (uint64_t)(m - 1)))
                break;
            at[m] = (uint32_t)j;
            flags |= gs[j].sg_Flags;
        }
        if (m < ncomp)
            continue;
        if (glyph >= l->f->nglyphs)
            return bad(l);

        /* The ligature in the first's place, the rest taken out, and
         * everything it spans one cluster: the first's. */
        uint32_t last = at[ncomp - 1], out = i + 1, next = 1;

        gs[i].sg_Glyph = glyph;
        gs[i].sg_Flags = flags & ~GLYPHF_HIDDEN;
        for (uint32_t j = i + 1; j < run->gr_Count; j++) {
            if (next < ncomp && j == at[next]) {
                next++;
                continue;
            }
            if (j <= last)
                gs[j].sg_Cluster = gs[i].sg_Cluster;
            gs[out++] = gs[j];
        }
        run->gr_Count = out;
        return true;
    }
    return false;
}

int64_t font_substitute(const struct font *f, struct GlyphRun *run, uint32_t features, uint32_t *warn)
{
    struct lay l = { f, run, 0, { nullptr, 0 }, true, false };
    uint32_t tags[3], ntags = 0, n;
    uint16_t lookups[MAX_LOOKUPS];
    int64_t e;

    if (features & FONT_FEAT_LIGA)
        tags[ntags++] = TAG('l', 'i', 'g', 'a');
    if (features & FONT_FEAT_TNUM)
        tags[ntags++] = TAG('t', 'n', 'u', 'm');
    if (features & FONT_FEAT_PNUM)
        tags[ntags++] = TAG('p', 'n', 'u', 'm');
    if ((e = gather(&l, f->gsub, tags, ntags, lookups, &n)) != 0)
        return e;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t type, nsub;
        struct Span lk = lookup(&l, f->gsub, lookups[k], &type, &nsub);

        if (l.bad)
            return ERR_BADFONT;
        if (type != 1 && type != 4 && type != 7) {
            *warn |= FONT_WARN_STUB_SHAPER;
            continue;
        }
        for (uint32_t i = 0; i < run->gr_Count; i++) {
            if (ignored(&l, i))
                continue;
            for (uint32_t s = 0; s < nsub; s++) {
                uint16_t real;
                struct Span sub = subtable(&l, lk, type, 7, s, &real);

                if (l.bad)
                    return ERR_BADFONT;
                if (real == 1 ? single(&l, sub, i) : real == 4 ? ligature(&l, sub, i) : false)
                    break;
                if (real != 1 && real != 4)
                    *warn |= FONT_WARN_STUB_SHAPER;
            }
            if (l.bad)
                return ERR_BADFONT;
        }
    }
    return 0;
}

/* --- GPOS and kern --- */

/* A value record's size, and its adjustments added to a glyph. */
static uint32_t value_size(uint16_t format)
{
    return 2 * (uint32_t)__builtin_popcount(format & 0xff);
}

static void adjust(struct lay *l, struct Span s, uint64_t off, uint16_t format, struct ShapedGlyph *sg)
{
    if (format & 1) {
        sg->sg_XOffset += (int16_t)g16(l, s, off);
        off += 2;
    }
    if (format & 2) {
        sg->sg_YOffset += (int16_t)g16(l, s, off);
        off += 2;
    }
    if (format & 4)
        sg->sg_Advance += (int16_t)g16(l, s, off);
    /* YAdvance and the device tables: not for horizontal text unhinted */
}

/* A pair adjustment at glyph i: the glyph to go on from in *next. */
static bool pair(struct lay *l, struct Span s, uint32_t i, uint32_t *next)
{
    struct ShapedGlyph *gs = l->run->gr_Glyphs;
    uint16_t fmt = g16(l, s, 0), vf1 = g16(l, s, 4), vf2 = g16(l, s, 6);
    uint32_t sz1 = value_size(vf1), sz2 = value_size(vf2);
    int64_t c = coverage(l, from(l, s, g16(l, s, 2)), gs[i].sg_Glyph), j;

    if (c < 0 || (j = next_glyph(l, i)) < 0 || gs[j].sg_Glyph >= CONTROL)
        return false;
    if (fmt == 1) {
        if (c >= g16(l, s, 8) || !fits(l, s, 10, (uint64_t)c + 1, 2))
            return bad(l);
        struct Span set = from(l, s, g16(l, s, 10 + 2 * (uint64_t)c));
        uint16_t n = g16(l, set, 0);
        uint32_t rec = 2 + sz1 + sz2, lo = 0, hi = n;

        if (!fits(l, set, 2, n, rec))
            return false;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint16_t second = g16(l, set, 2 + (uint64_t)mid * rec);

            if (second == gs[j].sg_Glyph) {
                adjust(l, set, 2 + (uint64_t)mid * rec + 2, vf1, &gs[i]);
                adjust(l, set, 2 + (uint64_t)mid * rec + 2 + sz1, vf2, &gs[j]);
                *next = (uint32_t)j + (vf2 ? 1 : 0);
                return true;
            }
            if (second < gs[j].sg_Glyph)
                lo = mid + 1;
            else
                hi = mid;
        }
        return false;
    }
    if (fmt != 2)
        return bad(l);
    uint32_t c1 = class_of(l, from(l, s, g16(l, s, 8)), gs[i].sg_Glyph);
    uint32_t c2 = class_of(l, from(l, s, g16(l, s, 10)), gs[j].sg_Glyph);
    uint16_t n1 = g16(l, s, 12), n2 = g16(l, s, 14);
    uint64_t rec = 16 + ((uint64_t)c1 * n2 + c2) * (sz1 + sz2);

    if (c1 >= n1 || c2 >= n2)
        return false;
    if (!fits(l, s, rec, 1, sz1 + sz2))
        return false;
    adjust(l, s, rec, vf1, &gs[i]);
    adjust(l, s, rec + sz1, vf2, &gs[j]);
    *next = (uint32_t)j + (vf2 ? 1 : 0);
    return true;
}

/* The kern table's format 0 subtables, horizontal and not cross-stream
 * or minimums, each over the run in turn: a pair's value added to the
 * first's advance. Marks are passed over, as by GPOS's IgnoreMarks. */
static int64_t kern_table(struct lay *l)
{
    struct Span k = l->f->kern;
    struct GlyphRun *run = l->run;
    uint16_t ntab = g16(l, k, 2);
    uint64_t at = 4;

    if (g16(l, k, 0) != 0)              /* Apple's version 1: not read */
        return 0;
    l->flags = IGNORE_MARKS;
    for (uint32_t t = 0; t < ntab && !l->bad; t++) {
        uint16_t len = g16(l, k, at + 2), cov = g16(l, k, at + 4);
        struct Span sub = from(l, k, at);
        uint16_t npairs = g16(l, sub, 6);

        if (l->bad || len < 6)
            return ERR_BADFONT;
        at += len;
        if (cov >> 8 != 0 || (cov & 7) != 1)
            continue;
        if (!fits(l, sub, 14, npairs, 6))
            return ERR_BADFONT;
        for (uint32_t i = 0; i < run->gr_Count;) {
            int64_t j;
            uint32_t want, lo = 0, hi = npairs;

            if (run->gr_Glyphs[i].sg_Glyph >= CONTROL || (j = next_glyph(l, i)) < 0 ||
                run->gr_Glyphs[j].sg_Glyph >= CONTROL) {
                i++;
                continue;
            }
            want = run->gr_Glyphs[i].sg_Glyph << 16 | run->gr_Glyphs[j].sg_Glyph;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2, have = g32(l, sub, 14 + 6 * (uint64_t)mid);

                if (have == want) {
                    run->gr_Glyphs[i].sg_Advance += (int16_t)g16(l, sub, 14 + 6 * (uint64_t)mid + 4);
                    break;
                }
                if (have < want)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            i = (uint32_t)j;
        }
    }
    return l->bad ? ERR_BADFONT : 0;
}

int64_t font_position(const struct font *f, struct GlyphRun *run, uint32_t features, uint32_t *warn)
{
    struct lay l = { f, run, 0, { nullptr, 0 }, false, false };
    uint32_t kern = TAG('k', 'e', 'r', 'n'), n;
    uint16_t lookups[MAX_LOOKUPS];
    int64_t e;

    if (!(features & FONT_FEAT_KERN))
        return 0;
    if (!f->gpos.sp_Len || !has_feature(&l, f->gpos, kern)) {
        if (l.bad)
            return ERR_BADFONT;
        return f->kern.sp_Len ? kern_table(&l) : 0;
    }
    if ((e = gather(&l, f->gpos, &kern, 1, lookups, &n)) != 0)
        return e;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t type, nsub;
        struct Span lk = lookup(&l, f->gpos, lookups[k], &type, &nsub);

        if (l.bad)
            return ERR_BADFONT;
        if (type != 2 && type != 9) {
            *warn |= FONT_WARN_STUB_SHAPER;
            continue;
        }
        for (uint32_t i = 0; i < run->gr_Count;) {
            uint32_t next = i + 1;

            if (!ignored(&l, i)) {
                for (uint32_t s = 0; s < nsub; s++) {
                    uint16_t real;
                    struct Span sub = subtable(&l, lk, type, 9, s, &real);

                    if (l.bad)
                        return ERR_BADFONT;
                    if (real != 2) {
                        *warn |= FONT_WARN_STUB_SHAPER;
                        continue;
                    }
                    if (pair(&l, sub, i, &next))
                        break;
                }
            }
            if (l.bad)
                return ERR_BADFONT;
            i = next;
        }
    }
    return 0;
}
