/*
 * tools/font-fuzz/fuzz.c -- font.library fuzzed (v8-plan steps 1 and 2, §14.5's pass 5)
 *
 * libFuzzer, with ASan and UBSan, from small seed fonts
 * (tools/font-fuzz/seeds, made by tools/font-subset.py). Each input is a
 * font, copied to a block of exactly its size, so a read one byte past
 * it is ASan's to catch. If it opens, everything a caller can ask of it
 * is asked: the metrics, the names, characters' glyphs, and each glyph's
 * metrics, its points, its outline at 16 px and its coverage; and text
 * shaped with each feature (the seeds have kerning pairs, ligatures and
 * figures, and one a kern table and GSUB behind extensions, so GSUB,
 * GPOS, GDEF and kern are what's mutated), some of it the input's own
 * bytes as UTF-8, then broken into lines. Every answer must be one the
 * API allows, and each call ends: a loop that didn't would be
 * libFuzzer's timeout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sfnt.h"

static void check_at(bool cond, int line)
{
    if (!cond) {
        fprintf(stderr, "font-fuzz: fuzz.c:%d doesn't hold\n", line);
        abort();
    }
}

#define CHECK(cond) check_at((cond), __LINE__)

static bool allowed(int64_t e)
{
    return e == 0 || e == ERR_BADFONT || e == ERR_UNSUPPORTED || e == ERR_FULL;
}

struct counter {
    struct font_sink s;
    unsigned n;
};

static bool count_point(struct font_sink *s, int32_t, int32_t, bool)
{
    return ++((struct counter *)s)->n <= 20000;
}

static bool count_end(struct font_sink *)
{
    return true;
}

/* One text shaped with features 'k' (of four), and broken into lines. */
static void shape(const struct FontFace *face, const struct FontMetrics *m, const char *text, size_t len, unsigned k)
{
    static struct ShapedGlyph glyphs[512];
    static const uint32_t features[] = { FONT_FEAT_KERN | FONT_FEAT_LIGA, FONT_FEAT_KERN | FONT_FEAT_TNUM,
                                         FONT_FEAT_LIGA | FONT_FEAT_PNUM, 0 };
    struct GlyphRun run = { glyphs, 512, 0, 0, 0 };
    int64_t e = ShapeRun(face, text, len, features[k % 4], &run), sum = 0;

    CHECK(e == 0 || e == ERR_BADFONT || e == ERR_UNSUPPORTED || (e == ERR_FULL && len > 512));
    if (e != 0) {
        CHECK(run.gr_Count == 0);
        return;
    }
    CHECK(run.gr_Count <= len);
    for (uint32_t i = 0; i < run.gr_Count; i++) {
        CHECK(glyphs[i].sg_Glyph < m->fm_NumGlyphs);
        CHECK(glyphs[i].sg_Cluster < len && (i == 0 || glyphs[i].sg_Cluster >= glyphs[i - 1].sg_Cluster));
        CHECK(!(glyphs[i].sg_Flags & ~0x3fu));
        sum += glyphs[i].sg_Advance;
    }
    CHECK(sum == run.gr_Advance);
    for (uint32_t at = 0; at < run.gr_Count;) {
        struct LineFit fit;

        FitLine(face, &run, at, 40 << 6, 13 << 6, &fit);
        CHECK(fit.lf_Count > 0 && at + fit.lf_Count <= run.gr_Count);
        at += fit.lf_Count;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static struct GfxPoint pts[2048];
    static uint8_t verbs[2048];
    static uint8_t mask[128 * 128];
    static int32_t scratch[130 * 128];
    uint8_t *copy = malloc(size ? size : 1);
    struct FontFace face;
    struct FontMetrics m;
    char name[64];
    int64_t e;

    memcpy(copy, data, size);
    e = OpenFont(&face, (struct Span){ copy, size });
    CHECK(e == 0 || e == ERR_BADFONT || e == ERR_UNSUPPORTED);
    if (e != 0) {
        free(copy);
        return 0;
    }
    FontInfo(&face, &m);
    for (uint32_t id = 0; id < 20; id++) {
        e = FontName(&face, id, name, sizeof name);
        CHECK(e >= 0 || e == ERR_NOTFOUND || e == ERR_BADFONT);
        CHECK(e < 0 || strlen(name) < sizeof name);
    }
    for (uint32_t c = 0; c < 0x300; c++)
        CHECK(MapCodepoint(&face, c) < m.fm_NumGlyphs);
    for (size_t i = 0; i + 4 <= size && i < 256; i += 4)
        CHECK(MapCodepoint(&face, (uint32_t)data[i] << 24 | (uint32_t)data[i + 1] << 16 |
                                      (uint32_t)data[i + 2] << 8 | data[i + 3]) < m.fm_NumGlyphs);
    for (uint32_t g = 0; g < m.fm_NumGlyphs && g < 64; g++) {
        struct GlyphMetrics gm;
        struct counter cn = { { count_point, count_end }, 0 };
        struct GfxPath path = { pts, verbs, 2048, 2048, 0, 0 };
        struct GfxRect r;

        CHECK(GetGlyphMetrics(&face, g, &gm) == 0);
        CHECK(allowed(font_glyph_points(font_of(&face), g, 0x10000, &cn.s)));
        e = GlyphOutline(&face, g, 16 << 6, &path);
        CHECK(allowed(e));
        CHECK(e == 0 || (path.gp_NumPoints == 0 && path.gp_NumVerbs == 0));
        if (e == 0 && PathBounds(&path, &r) && r.MaxX - r.MinX < 128 && r.MaxY - r.MinY < 128) {
            uint32_t w = (uint32_t)(r.MaxX - r.MinX + 1), h = (uint32_t)(r.MaxY - r.MinY + 1);
            struct GfxMask mk = { mask, w, h, w, r.MinX, r.MinY };

            CHECK(PathMask(&path, g & 1 ? PATHF_EVENODD : 0, &mk, scratch, sizeof scratch));
            /* A mask through the middle of it, its edges cut where the
             * outline crosses them (FillPath's pieces). */
            mk = (struct GfxMask){ mask, w / 2 + 1, h / 2 + 1, w / 2 + 1, r.MinX + (int32_t)w / 3,
                                   r.MinY + (int32_t)h / 3 };
            CHECK(PathMask(&path, g & 1 ? 0 : PATHF_EVENODD, &mk, scratch, sizeof scratch));
        }
    }
    CHECK(GetGlyphMetrics(&face, m.fm_NumGlyphs, &(struct GlyphMetrics){ 0 }) == ERR_BADARG);

    static const char *const texts[] = {
        "AV To ffi fl 1234 \xc3\xa9% A\xc2\xadV f\xe2\x80\x8c" "f",
        "\t\r\nAV\xe2\x80\xa8T.o \xe2\x80\x89 \xe2\x80\x91 \xd7\xa9 e\xcc\x81 \xed\xa0\x80",
    };
    struct LineSpacing ls;

    LineSpacing(&face, &ls);
    for (unsigned t = 0; t < sizeof texts / sizeof texts[0]; t++)
        shape(&face, &m, texts[t], strlen(texts[t]), (unsigned)size + t);
    shape(&face, &m, (const char *)data + size / 2, size - size / 2 < 600 ? size - size / 2 : 600, (unsigned)size + 2);
    free(copy);
    return 0;
}
