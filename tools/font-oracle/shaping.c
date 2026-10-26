/*
 * tools/font-oracle/shaping.c -- pass 2: ShapeRun against HarfBuzz (v8-plan step 2)
 *
 * docs §14.5's pass 2. Each font in the corpus shapes the same text
 * through ShapeRun and hb_shape, and the two must agree exactly: the
 * glyphs, their clusters, and where each is drawn (the pen plus its
 * offset), in the font's units. HarfBuzz is told what ShapeRun builds:
 * Latin, English, characters as clusters, and kern, liga, tnum and pnum
 * as the run's features ask, every other feature it would apply by
 * default off (ccmp, locl, calt, mark and the rest, which §14.4 stubs).
 *
 * The text: §14.5's kerning torture pairs, ligatures, the HUD's
 * punctuation, figures with tnum on and off, the spaces and hyphens
 * the fonts haven't all got, soft hyphens and joiners inside pairs and
 * ligatures; then every pair of a hundred-odd characters, for every
 * kerning pair a font has among them. And the line's spacing, as
 * HarfBuzz reads USE_TYPO_METRICS.
 *
 * What HarfBuzz can't be asked is checked alone (shaping_alone): the
 * controls, UTF-8 that's ill-formed (Unicode's maximal subparts, where
 * HarfBuzz gives a U+FFFD a byte), the stubs' warnings, ERR_FULL, and
 * FitLine's lines, which must each fit, break where a line may, and be
 * as long as fits.
 */
#include <stdlib.h>
#include <string.h>

#include <hb-ot.h>
#include <hb.h>

#include "oracle.h"

constexpr uint32_t MAX_GLYPHS = 4096;

static struct ShapedGlyph glyphs[MAX_GLYPHS];
static unsigned long shaped, pairs_kerned;

/* HarfBuzz's default features that ShapeRun hasn't: off. */
static const char *const not_built[] = {
    "rvrn", "ltra", "ltrm", "frac", "numr", "dnom", "rand", "abvm", "blwm", "ccmp",
    "locl", "mark", "mkmk", "rlig", "calt", "clig", "curs", "dist", "rclt", "trak",
};

static const struct {
    uint32_t features;
    const char *name;
} feature_sets[] = {
    { FONT_FEAT_KERN | FONT_FEAT_LIGA, "kern liga" },
    { 0, "none" },
    { FONT_FEAT_KERN | FONT_FEAT_TNUM, "kern tnum" },
    { FONT_FEAT_KERN | FONT_FEAT_LIGA | FONT_FEAT_PNUM, "kern liga pnum" },
    { FONT_FEAT_TNUM | FONT_FEAT_PNUM, "tnum pnum" },
};

static const char *const torture[] = {
    "To WA VA Yo We Av T. LT P. F, Y. f\u201d \u2018A\u2019",
    "fi fl ffi ffl ff office affluent waffle fjord",
    "12:38 PM \u2014 CPU 0.8% \u2022 RAM 0.00% \u2026",
    "0123456789 1111 $1,234.50 \u00a3 \u20ac9.99 \u00b110\u00b0",
    "AVAWAY LTA Tr\u00e8s Vo\u00fbte K\u00f6ln Yv\u00e9 P\u00e2 \u201cQuoted\u201d \u2018single\u2019 it\u2019s",
    "Caf\u00e9 na\u00efve fa\u00e7ade \u0152uvre \u00c6\u00d8\u00c5 \u00df \u00ff \u00a9 \u00ae \u2122 \u00a7 \u00b6 \u00d7 \u00f7",
    "\u2010 \u2012 \u2013 \u2014 \u2015 \u2016 \u2020 \u2021 \u2022 \u2026 \u2030 \u2039\u203a \u00ab\u00bb",
    "A\u00adV f\u00adi hy\u00adphen\u00adation T\u00ado",
    "a\u2009b\u202fc\u00a0d\u2002e\u2003f\u200ag\u2007h\u2008i\u2000j\u2001k\u2004l\u2005m\u2006n\u205fo",
    "non\u2011breaking A\u200bV a\u2060V \ufeffTo",
    "\u250c\u2500\u252c\u2500\u2510\u2502 \u2502\u2514\u2500\u2534\u2500\u2518\u2591\u2592\u2593\u2588 \u2580\u2584",
    "f\u200ci A\u200cV f\u200di A\u200dV f\u200cf f\u200df f\u00adf",
    "Wolf\u00adgang Va\u00ad\u00adVa",
};

/* The characters every pair of which is shaped. */
static const char pair_chars[] =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
    "\u00c0\u00c1\u00c4\u00c5\u00c6\u00c7\u00c9\u00d1\u00d6\u00d8\u00dc\u00df\u00e0\u00e1\u00e4\u00e5\u00e6"
    "\u00e7\u00e9\u00f1\u00f6\u00f8\u00fc\u201c\u201d\u2018\u2019\u2014\u2013\u2026\u2022";

static void hb_features(uint32_t features, hb_feature_t *out, unsigned *n)
{
    static const struct {
        uint32_t flag;
        const char *tag;
    } ours[] = { { FONT_FEAT_KERN, "kern" }, { FONT_FEAT_LIGA, "liga" }, { FONT_FEAT_TNUM, "tnum" },
                 { FONT_FEAT_PNUM, "pnum" } };

    *n = 0;
    for (unsigned i = 0; i < sizeof not_built / sizeof not_built[0]; i++)
        out[(*n)++] = (hb_feature_t){ hb_tag_from_string(not_built[i], 4), 0, HB_FEATURE_GLOBAL_START,
                                      HB_FEATURE_GLOBAL_END };
    for (unsigned i = 0; i < 4; i++)
        out[(*n)++] = (hb_feature_t){ hb_tag_from_string(ours[i].tag, 4), (features & ours[i].flag) ? 1u : 0u,
                                      HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END };
}

static void show(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++)
        fprintf(stderr, text[i] >= 0x20 && text[i] < 0x7f ? "%c" : "\\x%02x", (uint8_t)text[i]);
}

/* One text, both ways: the number of differences. */
static unsigned compare(const char *font, const struct FontFace *face, hb_font_t *hb, const char *text, size_t len,
                        unsigned set)
{
    struct GlyphRun run = { glyphs, MAX_GLYPHS, 0, 0, 0 };
    hb_buffer_t *buf = hb_buffer_create();
    hb_feature_t feats[32];
    unsigned nf, n, diffs = 0;
    int64_t e = ShapeRun(face, text, len, feature_sets[set].features, &run);
    int64_t ours = 0, theirs = 0;

    hb_buffer_add_utf8(buf, text, (int)len, 0, (int)len);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));
    hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    hb_features(feature_sets[set].features, feats, &nf);
    hb_shape(hb, buf, feats, nf);

    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, nullptr);

    shaped++;
    CHECK(e == 0, "%s: ShapeRun %lld", font, (long long)e);
    if (e == 0 && run.gr_Count != n) {
        CHECK(false, "%s [%s]: %u glyphs, HarfBuzz %u, for \"", font, feature_sets[set].name, run.gr_Count, n);
        if (failures <= 40)
            show(text, len), fputs("\"\n", stderr);
        diffs++;
    }
    for (unsigned i = 0; e == 0 && !diffs && i < n; i++) {
        const struct ShapedGlyph *g = &glyphs[i];

        /* A hidden glyph isn't drawn, so where isn't asked: HarfBuzz
         * splits a kern table's value over the pair, and the pen
         * between them, at a soft hyphen, is half way. */
        bool where = !(g->sg_Flags & GLYPHF_HIDDEN);

        if (g->sg_Glyph != info[i].codepoint || g->sg_Cluster != info[i].cluster ||
            (where && (ours + g->sg_XOffset != theirs + pos[i].x_offset || g->sg_YOffset != pos[i].y_offset))) {
            CHECK(false, "%s [%s]: glyph %u: %u at %u, drawn at %lld,%d; HarfBuzz %u at %u, %lld,%d, in \"", font,
                  feature_sets[set].name, i, g->sg_Glyph, g->sg_Cluster, (long long)(ours + g->sg_XOffset),
                  g->sg_YOffset, info[i].codepoint, info[i].cluster, (long long)(theirs + pos[i].x_offset),
                  pos[i].y_offset);
            if (failures <= 40)
                show(text, len), fputs("\"\n", stderr);
            diffs++;
            break;
        }
        ours += g->sg_Advance;
        theirs += pos[i].x_advance;
    }
    if (e == 0 && !diffs && ours != theirs) {
        CHECK(false, "%s [%s]: the run's width %lld, HarfBuzz %lld", font, feature_sets[set].name, (long long)ours,
              (long long)theirs);
        diffs++;
    }
    CHECK(e != 0 || diffs || run.gr_Advance == ours, "%s: gr_Advance %d isn't the advances' sum %lld", font, run.gr_Advance,
          (long long)ours);
    hb_buffer_destroy(buf);
    return diffs;
}

/* The run's width, ShapeRun's, for a text. */
static int32_t width_of(const struct FontFace *face, const char *text, uint32_t features)
{
    struct GlyphRun run = { glyphs, MAX_GLYPHS, 0, 0, 0 };

    return ShapeRun(face, text, strlen(text), features, &run) == 0 ? run.gr_Advance : -1;
}

static uint32_t count_of(const struct FontFace *face, const char *text, uint32_t features)
{
    struct GlyphRun run = { glyphs, MAX_GLYPHS, 0, 0, 0 };

    return ShapeRun(face, text, strlen(text), features, &run) == 0 ? run.gr_Count : 0;
}

void shaping(const char *font, const uint8_t *buf, size_t len, const struct FontFace *face)
{
    hb_blob_t *blob = hb_blob_create((const char *)buf, (unsigned)len, HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t *hbface = hb_face_create(blob, 0);
    hb_font_t *hb = hb_font_create(hbface);
    unsigned fails0 = failures, diffs = 0, texts = 0;
    unsigned long kerned0 = pairs_kerned;
    struct FontMetrics m;
    struct LineSpacing ls;
    hb_font_extents_t ext;

    FontInfo(face, &m);
    hb_font_set_scale(hb, m.fm_UnitsPerEm, m.fm_UnitsPerEm);

    LineSpacing(face, &ls);
    hb_font_get_h_extents(hb, &ext);
    CHECK(ls.ls_Ascent == ext.ascender && ls.ls_Descent == -ext.descender && ls.ls_LineGap == ext.line_gap,
          "%s: LineSpacing %d %d %d, HarfBuzz %d %d %d", font, ls.ls_Ascent, ls.ls_Descent, ls.ls_LineGap,
          ext.ascender, -ext.descender, ext.line_gap);

    for (unsigned t = 0; t < sizeof torture / sizeof torture[0]; t++)
        for (unsigned s = 0; s < sizeof feature_sets / sizeof feature_sets[0]; s++, texts++)
            diffs += compare(font, face, hb, torture[t], strlen(torture[t]), s);

    /* Every pair: one text a first character, each second after it. */
    const char *chars = pair_chars, *end = chars + strlen(chars);

    for (const char *a = chars; a < end;) {
        size_t la = 1;
        char text[2048];
        size_t n = 0;

        while (a + la < end && ((uint8_t)a[la] & 0xc0) == 0x80)
            la++;
        for (const char *b = chars; b < end;) {
            size_t lb = 1;

            while (b + lb < end && ((uint8_t)b[lb] & 0xc0) == 0x80)
                lb++;
            memcpy(text + n, a, la), n += la;
            memcpy(text + n, b, lb), n += lb;
            b += lb;
        }
        for (unsigned s = 0; s < 2; s++, texts++)
            diffs += compare(font, face, hb, text, n, s);
        a += la;
    }
    for (const char *a = "AVTWYLPFT"; *a; a++)        /* to say the pairs were kerned at all */
        for (const char *b = "AVoaey.,"; *b; b++) {
            char two[3] = { *a, *b, 0 }, one_a[2] = { *a, 0 }, one_b[2] = { *b, 0 };

            if (width_of(face, two, FONT_FEAT_KERN) !=
                width_of(face, one_a, FONT_FEAT_KERN) + width_of(face, one_b, FONT_FEAT_KERN))
                pairs_kerned++;
        }

    /* What the corpus is for: liga in Source Sans 3 (ff, ft, fft), and
     * in KernTest behind an extension; tnum in Inter; kerning in all but
     * the monospaced, KernTest's from its kern table. */
    if (!strstr(font, "Mono"))
        CHECK(pairs_kerned > kerned0, "%s: nothing kerned", font);
    if (strstr(font, "KernTest"))
        CHECK(!font_of(face)->gpos.sp_Len && font_of(face)->kern.sp_Len, "%s: not a kern table's", font);
    if (strstr(font, "SourceSans3") || strstr(font, "KernTest"))
        CHECK(count_of(face, "ffi fl", FONT_FEAT_LIGA) == 5 && count_of(face, "ffi fl", 0) == 6,
              "%s: no ligatures", font);
    if (strstr(font, "Inter") || strstr(font, "SystemSans"))
        CHECK(width_of(face, "1111", FONT_FEAT_TNUM) != width_of(face, "1111", 0), "%s: no tabular figures", font);
    if (failures == fails0)
        printf("font-oracle: ok: pass 2: %s: %u texts in %zu feature sets, as HarfBuzz shapes them; "
               "%lu of 72 pairs kerned\n", font, texts / 1, sizeof feature_sets / sizeof feature_sets[0],
               pairs_kerned - kerned0);
    (void)diffs;
    hb_font_destroy(hb);
    hb_face_destroy(hbface);
    hb_blob_destroy(blob);
}

/* --- What HarfBuzz isn't asked --- */

static void open_system(uint32_t which, struct FontFace *face)
{
    struct Span file;

    if (!SystemFont(which, &file) || OpenFont(face, file) != 0) {
        fprintf(stderr, "font-oracle: system font %#x won't open\n", which);
        exit(2);
    }
}

/* Each glyph's cluster and flags, for text whose characters are known. */
static void expect(const struct FontFace *face, const char *text, size_t len, uint32_t n, const uint32_t *clusters,
                   const uint32_t *flags, const uint32_t *cps, const char *what)
{
    struct GlyphRun run = { glyphs, MAX_GLYPHS, 0, 0, 0 };
    int64_t e = ShapeRun(face, text, len, FONT_FEAT_KERN | FONT_FEAT_LIGA, &run);

    CHECK(e == 0 && run.gr_Count == n, "%s: %lld, %u glyphs, not %u", what, (long long)e, run.gr_Count, n);
    for (uint32_t i = 0; e == 0 && i < n && i < run.gr_Count; i++) {
        CHECK(glyphs[i].sg_Cluster == clusters[i], "%s: glyph %u's cluster %u, not %u", what, i,
              glyphs[i].sg_Cluster, clusters[i]);
        CHECK(!flags || glyphs[i].sg_Flags == flags[i], "%s: glyph %u's flags %#x, not %#x", what, i,
              glyphs[i].sg_Flags, flags[i]);
        CHECK(!cps || glyphs[i].sg_Glyph == MapCodepoint(face, cps[i]), "%s: glyph %u is %u, not U+%04X's", what, i,
              glyphs[i].sg_Glyph, cps[i]);
        CHECK(!(glyphs[i].sg_Flags & GLYPHF_HIDDEN) || (glyphs[i].sg_Advance == 0 && glyphs[i].sg_Glyph ==
                                                                                           MapCodepoint(face, ' ')),
              "%s: hidden glyph %u has an advance, or isn't a space", what, i);
    }
}

static void controls_and_utf8(void)
{
    struct FontFace sans, mono;

    open_system(FONT_SANS, &sans);
    open_system(FONT_MONO, &mono);

    /* Tab, CR LF (one newline, LF's), a CR alone, NEL, U+2028. */
    static const char ctl[] = "a\tb\r\nc\rd\xc2\x85" "e\xe2\x80\xa8";
    static const uint32_t ctl_clusters[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11 };
    static const uint32_t ctl_flags[] = { 0, GLYPHF_TAB | GLYPHF_HIDDEN, 0, GLYPHF_HIDDEN,
                                          GLYPHF_NEWLINE | GLYPHF_HIDDEN, 0, GLYPHF_NEWLINE | GLYPHF_HIDDEN, 0,
                                          GLYPHF_NEWLINE | GLYPHF_HIDDEN, 0, GLYPHF_NEWLINE | GLYPHF_HIDDEN };
    expect(&sans, ctl, sizeof ctl - 1, 11, ctl_clusters, ctl_flags, nullptr, "controls");

    /* Ill-formed UTF-8: one U+FFFD for each maximal subpart. A cut
     * three-byte sequence; a surrogate (three); an overlong (two); past
     * U+10FFFF (four); a lone continuation; and a good four-byte one. */
    static const char bad[] = "\xe2\x82 \xed\xa0\x80\xc0\xaf\xf4\x90\x80\x80\x80\xf0\x9f\x98\x80";
    static const uint32_t bad_clusters[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    static const uint32_t bad_cps[] = { 0xfffd, ' ', 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd, 0xfffd,
                                        0xfffd, 0xfffd, 0xfffd, 0xfffd, 0x1f600 };
    expect(&mono, bad, sizeof bad - 1, 13, bad_clusters, nullptr, bad_cps, "UTF-8");

    /* Soft hyphen, ZWSP: there, hidden, where a line may break. */
    static const uint32_t shy_clusters[] = { 0, 1, 3, 4 };
    static const uint32_t shy_flags[] = { 0, GLYPHF_SOFT_HYPHEN | GLYPHF_HIDDEN, 0, GLYPHF_SPACE | GLYPHF_HIDDEN };
    expect(&sans, "a\xc2\xad" "b\xe2\x80\x8b", 7, 4, shy_clusters, shy_flags, nullptr, "soft hyphen");

    /* The stubs: Hebrew, Devanagari, a combining accent, an override. */
    static const struct {
        const char *text;
        uint32_t count, warnings;
    } stubs[] = {
        { "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 4, FONT_WARN_STUB_SHAPER | FONT_WARN_STUB_BIDI },
        { "\xe0\xa4\xa8\xe0\xa4\xae", 2, FONT_WARN_STUB_SHAPER },
        { "e\xcc\x81", 2, FONT_WARN_STUB_SHAPER },
        { "\xe2\x80\xae" "AV", 3, FONT_WARN_STUB_BIDI },
        { "AV", 2, 0 },
    };
    for (unsigned i = 0; i < sizeof stubs / sizeof stubs[0]; i++) {
        struct GlyphRun run = { glyphs, MAX_GLYPHS, 0, 0, 0 };
        int64_t e = ShapeRun(&sans, stubs[i].text, strlen(stubs[i].text), FONT_FEAT_KERN, &run);

        CHECK(e == 0 && run.gr_Count == stubs[i].count && run.gr_Warnings == stubs[i].warnings,
              "stub %u: %lld, %u glyphs, warnings %#x", i, (long long)e, run.gr_Count, run.gr_Warnings);
    }
    /* An override's run is still kerned; a Hebrew one isn't. */
    CHECK(width_of(&sans, "\xe2\x80\xae" "AV", FONT_FEAT_KERN) == width_of(&sans, "AV", FONT_FEAT_KERN) &&
              width_of(&sans, "AV", FONT_FEAT_KERN) < width_of(&sans, "AV", 0),
          "an override's run isn't kerned");
    CHECK(width_of(&sans, "AV\xd7\xa9", FONT_FEAT_KERN) == width_of(&sans, "AV\xd7\xa9", 0),
          "a Hebrew run is kerned");

    /* Too many characters: ERR_FULL, and nothing. */
    struct GlyphRun small = { glyphs, 3, 7, 7, 7 };

    CHECK(ShapeRun(&sans, "abcd", 4, 0, &small) == ERR_FULL && small.gr_Count == 0, "ERR_FULL");
    small.gr_Max = 4;
    CHECK(ShapeRun(&sans, "abcd", 4, 0, &small) == 0 && small.gr_Count == 4, "a run that just fits");
    CHECK(ShapeRun(&sans, "", 0, 0, &small) == 0 && small.gr_Count == 0 && small.gr_Advance == 0, "an empty run");
    CHECK(ShapeRun(&sans, "a", 1ull << 31, 0, &small) == ERR_BADARG, "a run of 2 GB");
}

/* --- FitLine --- */

static const char paragraph[] =
    "The quick brown fox jumps over the lazy dog. Hyphen\xc2\xad" "ation, a soft hy\xc2\xad" "phen, a well-known "
    "hard one; 12:38 PM \xe2\x80\x94 CPU 0.8%.\n\tTabbed, then a line\r\nafter CR LF. "
    "Averyveryverylongwordthatwillnotfitonanyreasonablylinewhatsoever and   spaces   between.  ";

static bool breakable(const struct ShapedGlyph *g)
{
    return g->sg_Flags & (GLYPHF_SPACE | GLYPHF_TAB | GLYPHF_NEWLINE | GLYPHF_HYPHEN | GLYPHF_SOFT_HYPHEN);
}

/* The ink's width of glyphs [from, to): to the end of the last one not a
 * space, tab or hidden. */
static int32_t ink(const struct GlyphRun *run, uint32_t from, uint32_t to)
{
    int32_t x = 0, last = 0;

    for (uint32_t i = from; i < to; i++) {
        x += run->gr_Glyphs[i].sg_Advance;
        if (!(run->gr_Glyphs[i].sg_Flags & (GLYPHF_SPACE | GLYPHF_TAB | GLYPHF_HIDDEN)))
            last = x;
    }
    return last;
}

static void lines(uint32_t which, uint32_t size)
{
    struct FontFace face;
    struct GlyphRun run = { glyphs, MAX_GLYPHS, 0, 0, 0 };
    unsigned nlines = 0, hyphens = 0, forced = 0, fails0 = failures;
    uint32_t hyphen;
    struct GlyphMetrics hm;

    open_system(which, &face);
    hyphen = MapCodepoint(&face, '-');
    GetGlyphMetrics(&face, hyphen, &hm);
    for (int32_t width = 20 << 6; width <= 700 << 6; width += 13 << 6) {
        uint32_t at = 0;
        bool ended = false;

        CHECK(ShapeRun(&face, paragraph, sizeof paragraph - 1, FONT_FEAT_KERN | FONT_FEAT_LIGA, &run) == 0,
              "FitLine's paragraph");
        while (at < run.gr_Count && failures < fails0 + 10) {
            struct LineFit fit;
            uint32_t end;
            int32_t w;

            FitLine(&face, &run, at, width, size, &fit);
            end = at + fit.lf_Count;
            nlines++;
            CHECK(fit.lf_Count > 0 && end <= run.gr_Count, "a line of %u glyphs at %u", fit.lf_Count, at);
            if (fit.lf_Count == 0 || end > run.gr_Count)
                break;
            w = ink(&run, at, end) + (fit.lf_Flags & LINEF_HYPHEN ? hm.gm_Advance : 0);
            CHECK(fit.lf_Width == ScaleUnits(&face, w, size), "line at %u: width %d, not %d", at, fit.lf_Width,
                  ScaleUnits(&face, w, size));
            CHECK(fit.lf_Width <= width || fit.lf_Count == 1, "line at %u: %d wider than %d", at, fit.lf_Width,
                  width);
            CHECK(fit.lf_Hyphen == hyphen, "lf_Hyphen");
            if (fit.lf_Flags & LINEF_FORCED) {
                forced++;
                /* mid-word: or after a soft hyphen whose hyphen didn't fit */
                CHECK(!(run.gr_Glyphs[end - 1].sg_Flags & (GLYPHF_SPACE | GLYPHF_TAB | GLYPHF_HYPHEN)),
                      "line at %u: cut after a break", at);
            } else if (!(fit.lf_Flags & LINEF_END) || end < run.gr_Count) {
                CHECK(breakable(&run.gr_Glyphs[end - 1]), "line at %u ends at glyph %u, not a break", at, end - 1);
            }
            if (fit.lf_Flags & LINEF_HYPHEN) {
                hyphens++;
                CHECK(run.gr_Glyphs[end - 1].sg_Flags & GLYPHF_SOFT_HYPHEN, "LINEF_HYPHEN not at a soft hyphen");
            }
            if (fit.lf_Flags & LINEF_NEWLINE)
                CHECK(run.gr_Glyphs[end - 1].sg_Flags & GLYPHF_NEWLINE, "LINEF_NEWLINE not at a newline");

            /* As long as fits: the next place it could break, past this
             * one, wouldn't have. */
            if (!(fit.lf_Flags & (LINEF_END | LINEF_NEWLINE | LINEF_FORCED))) {
                uint32_t k = end;

                while (k < run.gr_Count && (run.gr_Glyphs[k].sg_Flags & (GLYPHF_SPACE | GLYPHF_TAB)))
                    k++;
                while (k < run.gr_Count && !breakable(&run.gr_Glyphs[k]))
                    k++;
                if (k < run.gr_Count) {
                    int32_t more = ink(&run, at, k + 1);

                    if (run.gr_Glyphs[k].sg_Flags & GLYPHF_SOFT_HYPHEN)
                        more += hm.gm_Advance;
                    CHECK(ScaleUnits(&face, more, size) > width,
                          "line at %u of width %d stops at %u, but to %u (%d) fits", at, width, end, k,
                          ScaleUnits(&face, more, size));
                } else {
                    CHECK(ScaleUnits(&face, ink(&run, at, run.gr_Count), size) > width,
                          "line at %u could have taken the rest", at);
                }
            }
            if (fit.lf_Flags & LINEF_END)
                ended = end == run.gr_Count;
            at = end;
        }
        CHECK(ended, "width %d: the run's end wasn't LINEF_END", width);
    }
    /* A tab at a line's start is a whole stop: eight spaces. */
    struct GlyphMetrics sm;
    struct LineFit fit;

    GetGlyphMetrics(&face, MapCodepoint(&face, ' '), &sm);
    CHECK(ShapeRun(&face, "\tx", 2, 0, &run) == 0, "a tab");
    FitLine(&face, &run, 0, 1000 << 6, size, &fit);
    CHECK(run.gr_Glyphs[0].sg_Advance == 8 * sm.gm_Advance && run.gr_Advance == 8 * sm.gm_Advance +
                                                                                    run.gr_Glyphs[1].sg_Advance,
          "a tab's advance %d, not %d", run.gr_Glyphs[0].sg_Advance, 8 * sm.gm_Advance);
    CHECK(ShapeRun(&face, "x\tx", 3, 0, &run) == 0, "a tab");
    FitLine(&face, &run, 0, 1000 << 6, size, &fit);
    CHECK(run.gr_Glyphs[0].sg_Advance + run.gr_Glyphs[1].sg_Advance == 8 * sm.gm_Advance, "a tab after an x");
    FitLine(&face, &run, 3, 1000 << 6, size, &fit);
    CHECK(fit.lf_Count == 0 && fit.lf_Flags == LINEF_END, "a line past the run's end");
    CHECK(hyphens && forced, "the paragraph never broke at a soft hyphen (%u) or was cut (%u)", hyphens, forced);
    if (failures == fails0)
        printf("font-oracle: ok: FitLine: system font %#x at %u px, 53 widths: %u lines, %u at soft hyphens, "
               "%u cut, each fitting and as long as fits\n", which, size >> 6, nlines, hyphens, forced);
}

void shaping_alone(void)
{
    unsigned fails0 = failures;

    controls_and_utf8();
    if (failures == fails0)
        printf("font-oracle: ok: controls, ill-formed UTF-8, the stubs' warnings, ERR_FULL\n");
    lines(FONT_SANS, 13 << 6);
    lines(FONT_MONO | FONT_BOLD, 16 << 6);
    printf("font-oracle: pass 2: %lu texts shaped against HarfBuzz\n", shaped);
}
