/*
 * tools/font-fuzz/fuzz.c -- font.library fuzzed (v8-plan step 1, §14.5's pass 5)
 *
 * libFuzzer, with ASan and UBSan, from small seed fonts
 * (tools/font-fuzz/seeds, made by tools/font-subset.py). Each input is a
 * font, copied to a block of exactly its size, so a read one byte past
 * it is ASan's to catch. If it opens, everything a caller can ask of it
 * is asked: the metrics, the names, characters' glyphs, and each glyph's
 * metrics, its points, its outline at 16 px and its coverage. Every
 * answer must be one the API allows, and each call ends: a loop that
 * didn't would be libFuzzer's timeout.
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
        }
    }
    CHECK(GetGlyphMetrics(&face, m.fm_NumGlyphs, &(struct GlyphMetrics){ 0 }) == ERR_BADARG);
    free(copy);
    return 0;
}
