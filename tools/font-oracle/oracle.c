/*
 * tools/font-oracle/oracle.c -- font.library against FreeType and HarfBuzz (v8-plan steps 1 and 2)
 *
 * docs §14.5's oracle, on the host: lib/font and lib/gfx's rasteriser
 * built with the sanitizers and linked beside the host's FreeType and
 * HarfBuzz, only ever the reference here (§14.1: never read for the code).
 * Over every font in the corpus and the system fonts' subsets:
 *
 *   pass 1  metrics: head, hhea, OS/2 and post as FreeType reads them;
 *           each glyph's advance; every code point's glyph; the names.
 *           All exact.
 *   pass 2  shaping, against HarfBuzz (shaping.c).
 *   pass 3  outlines: each glyph's points, on or off the curve, and its
 *           contours, in font units (exact, or within a unit where a
 *           composite's matrix rounds), and at 16 px as FreeType scales
 *           them unhinted (exact in 26.6).
 *   pass 4  coverage: each glyph at 12, 13, 16, 24 and 48 px by PathMask,
 *           against the exact coverage (reference(), below): every pixel
 *           within 2/255, and 99.8% within 1/255, §14.5's numbers. And
 *           against FreeType unhinted: boxes within a pixel, and the
 *           pixels within the guards below. §14.5 held PathMask to
 *           FreeType itself, but FreeType's curves aren't exact (the
 *           guards' comment), so it can't be both.
 *
 * Then malformed fonts made from a subset by hand: cut short at every
 * length, a composite that holds itself, a glyph past glyf's end, a bad
 * magic, CFF and a collection. Each must be an error, never a fault.
 * (Pass 5, the fuzzer, is tools/font-fuzz.)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_ADVANCES_H

#include "fontcheck.h"
#include "oracle.h"

/*
 * Against FreeType: guards against gross error, not agreement. Its
 * unhinted renderer cuts curves coarser than PathMask's 1/512 px, so on
 * a curve's edge its pixels differ from the exact by up to a tenth
 * (measured 2026-10-26: worst 29/255, 65-80% within 1/255), and on
 * straight edges its rounding leaves 99.7-99.95% within 1/255, worst
 * 3/255. PathMask is held to the exact coverage instead (2/255).
 */
constexpr int STRAIGHT_WORST = 3;
constexpr double STRAIGHT_WITHIN1 = 99.5;
constexpr int CURVED_WORST = 40;
constexpr double CURVED_WITHIN1 = 60.0;

unsigned failures;

static uint8_t *load(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!fp || fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "font-oracle: can't read %s\n", path);
        exit(2);
    }
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n)
        exit(2);
    fclose(fp);
    *len = (size_t)n;
    return buf;
}

static const char *base(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

/* --- Points, as the file has them --- */

struct raw {
    struct font_sink s;
    int32_t x[8192], y[8192];
    uint8_t on[8192];
    int ends[1024];
    int n, ncont;
};

static bool raw_point(struct font_sink *s, int32_t x, int32_t y, bool on)
{
    struct raw *r = (struct raw *)s;

    if (r->n >= 8192)
        return false;
    r->x[r->n] = x, r->y[r->n] = y, r->on[r->n] = on;
    r->n++;
    return true;
}

static bool raw_end(struct font_sink *s)
{
    struct raw *r = (struct raw *)s;

    if (r->ncont >= 1024)
        return false;
    r->ends[r->ncont++] = r->n - 1;
    return true;
}

static struct raw raw = { .s = { raw_point, raw_end } };

/* Our points against FreeType's outline: exact, or within 'tol'. */
static void compare_points(const char *font, unsigned g, const FT_Outline *o, int tol, const char *what)
{
    int ncont = 0;

    /* FreeType drops a contour with no points; so do we, here. */
    for (int i = 0; i < raw.ncont; i++)
        if (i == 0 ? raw.ends[i] >= 0 : raw.ends[i] > raw.ends[i - 1])
            raw.ends[ncont++] = raw.ends[i];
    CHECK(raw.n == o->n_points && ncont == o->n_contours, "%s glyph %u %s: %d points %d contours, FreeType %d %d",
          font, g, what, raw.n, ncont, o->n_points, o->n_contours);
    if (raw.n != o->n_points || ncont != o->n_contours)
        return;
    for (int i = 0; i < ncont; i++)
        CHECK(raw.ends[i] == o->contours[i], "%s glyph %u %s: contour %d ends at %d, FreeType %d", font, g, what,
              i, raw.ends[i], o->contours[i]);
    for (int i = 0; i < raw.n; i++) {
        long dx = labs(raw.x[i] - o->points[i].x), dy = labs(raw.y[i] - o->points[i].y);
        bool on = FT_CURVE_TAG(o->tags[i]) == FT_CURVE_TAG_ON;

        CHECK(dx <= tol && dy <= tol && on == raw.on[i], "%s glyph %u %s: point %d (%d, %d)%s, FreeType (%ld, %ld)%s",
              font, g, what, i, raw.x[i], raw.y[i], raw.on[i] ? "" : " off", o->points[i].x, o->points[i].y,
              on ? "" : " off");
    }
}

static bool is_composite(const struct font *f, unsigned g)
{
    /* glyf's numberOfContours < 0, through loca. */
    uint32_t from;
    uint16_t a, n;

    if (f->loca_long)
        SpanReadU32BE(f->loca, 4 * (uint64_t)g, &from);
    else
        SpanReadU16BE(f->loca, 2 * (uint64_t)g, &a), from = 2u * a;
    return SpanReadU16BE(f->glyf, from, &n) && (int16_t)n < 0;
}

/* --- One font --- */

static unsigned long glyphs_drawn;

/* Pixels compared, those within 1/255, and the worst. */
struct stats {
    unsigned long n, in1;
    int worst;
};

static void count(struct stats *s, int a, int b)
{
    int d = abs(a - b);

    if (a == 0 && b == 0)
        return;
    s->n++;
    s->in1 += d <= 1;
    s->worst = d > s->worst ? d : s->worst;
}

static double pct(const struct stats *s)
{
    return s->n ? 100.0 * (double)s->in1 / (double)s->n : 100.0;
}

static struct stats all_exact, all_straight, all_curved;

/*
 * The exact coverage, as nearly as doubles get it: the path's curves cut
 * into lines within 1/4096 px, and each line's area added up as
 * PathMask does, but in floating point. Its 8-bit rounding is the only
 * error worth the name, so PathMask is held to 2/255 of it.
 */
static double ref_acc[514 * 512];

static void ref_line(int w, int h, double x0, double y0, double x1, double y1)
{
    double dir = 1;

    if (y0 == y1)
        return;
    if (y0 > y1) {
        double t;

        t = x0, x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
        dir = -1;
    }
    for (int row = (int)floor(y0 < 0 ? 0 : y0); row < h && row < y1; row++) {
        double ya = row > y0 ? row : y0, yb = row + 1 < y1 ? row + 1 : y1;
        double xa = x0 + (x1 - x0) * (ya - y0) / (y1 - y0), xb = x0 + (x1 - x0) * (yb - y0) / (y1 - y0);
        double *acc = ref_acc + (size_t)row * (size_t)(w + 2);

        xa = xa < 0 ? 0 : xa > w ? w : xa;
        xb = xb < 0 ? 0 : xb > w ? w : xb;
        /* Cut at each pixel edge between xa and xb. */
        double px = xa, py = ya;

        for (;;) {
            int c = (int)floor(xa < xb ? px : (px > xb ? nextafter(px, -1e9) : px));
            double edge = xa < xb ? c + 1 : c, qx, qy;

            if (xa == xb || (xa < xb ? edge >= xb : edge <= xb)) {
                qx = xb, qy = yb;
            } else {
                qx = edge;
                qy = ya + (yb - ya) * (qx - xa) / (xb - xa);
            }
            double dy = (qy - py) * dir, own = dy * (2 - (px - c) - (qx - c)) / 2;

            c = c > w ? w : c;
            acc[c] += own;
            acc[c + 1] += dy - own;
            if (qx == xb && qy == yb)
                break;
            px = qx, py = qy;
        }
    }
}

static void reference(const struct GfxPath *p, int w, int h, int ox, int oy, uint8_t *out)
{
    double sx = 0, sy = 0, cx = 0, cy = 0;
    const struct GfxPoint *pt = p->gp_Points;

    memset(ref_acc, 0, sizeof ref_acc);
#define RX(i) (pt[i].x / 64.0 - ox)
#define RY(i) (pt[i].y / 64.0 - oy)
    for (uint32_t v = 0; v <= p->gp_NumVerbs; v++) {
        uint8_t verb = v < p->gp_NumVerbs ? p->gp_Verbs[v] : PATH_CLOSE;

        if (verb == PATH_MOVE || verb == PATH_CLOSE) {
            ref_line(w, h, cx, cy, sx, sy);
            cx = sx, cy = sy;
            if (verb == PATH_MOVE)
                sx = cx = RX(0), sy = cy = RY(0), pt++;
        } else if (verb == PATH_LINE) {
            ref_line(w, h, cx, cy, RX(0), RY(0));
            cx = RX(0), cy = RY(0), pt++;
        } else if (verb == PATH_QUAD) {
            double bx = fabs(cx - 2 * RX(0) + RX(1)) + fabs(cy - 2 * RY(0) + RY(1));
            int n = (int)ceil(sqrt(bx / (4.0 / 4096))) + 1;
            double qx = cx, qy = cy;

            for (int i = 1; i <= n; i++) {
                double t = (double)i / n, u = 1 - t;
                double nx = u * u * cx + 2 * u * t * RX(0) + t * t * RX(1);
                double ny = u * u * cy + 2 * u * t * RY(0) + t * t * RY(1);

                ref_line(w, h, qx, qy, nx, ny);
                qx = nx, qy = ny;
            }
            cx = RX(1), cy = RY(1), pt += 2;
        }
    }
#undef RX
#undef RY
    for (int y = 0; y < h; y++) {
        double s = 0;

        for (int x = 0; x < w; x++) {
            s += ref_acc[(size_t)y * (size_t)(w + 2) + (size_t)x];
            out[y * w + x] = (uint8_t)lround(fmin(fabs(s), 1.0) * 255);
        }
    }
}

static void name_check(const char *font, FT_Face ft, const struct FontFace *face, unsigned id)
{
    char ours[256], theirs[256];
    int64_t n = FontName(face, id, ours, sizeof ours);
    FT_UInt count = FT_Get_Sfnt_Name_Count(ft);
    bool found = false;

    for (FT_UInt i = 0; i < count && !found; i++) {
        FT_SfntName nm;

        if (FT_Get_Sfnt_Name(ft, i, &nm) || nm.name_id != id || nm.platform_id != 3 || nm.encoding_id != 1 ||
            nm.language_id != 0x409)
            continue;
        size_t k = 0;

        for (FT_UInt j = 0; j + 1 < nm.string_len && k + 1 < sizeof theirs; j += 2) {
            unsigned c = (unsigned)nm.string[j] << 8 | nm.string[j + 1];

            theirs[k++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
        }
        theirs[k] = 0;
        found = true;
    }
    if (!found)
        return;                         /* not in the English Windows record: ours may find another */
    CHECK(n >= 0 && strcmp(ours, theirs) == 0, "%s name %u: \"%s\", FreeType \"%s\"", font, id, n >= 0 ? ours : "?",
          theirs);
}

static void one_font(FT_Library lib, const char *file)
{
    static const unsigned sizes[] = { 12, 13, 16, 24, 48 };
    const char *font = base(file);
    size_t len;
    uint8_t *buf = load(file, &len);
    struct FontFace face;
    struct FontMetrics m;
    FT_Face ft;
    unsigned fails0 = failures, cmap_checked = 0, composites = 0;
    int64_t e;

    if (FT_New_Memory_Face(lib, buf, (FT_Long)len, 0, &ft)) {
        fprintf(stderr, "font-oracle: FreeType can't open %s\n", file);
        exit(2);
    }
    e = OpenFont(&face, (struct Span){ buf, len });
    CHECK(e == 0, "%s: OpenFont %lld", font, (long long)e);
    if (e != 0)
        return;
    const struct font *f = font_of(&face);

    /* Pass 1: metrics, names, glyphs' advances, and the cmap. */
    TT_Header *head = FT_Get_Sfnt_Table(ft, FT_SFNT_HEAD);
    TT_HoriHeader *hhea = FT_Get_Sfnt_Table(ft, FT_SFNT_HHEA);
    TT_OS2 *os2 = FT_Get_Sfnt_Table(ft, FT_SFNT_OS2);
    TT_Postscript *post = FT_Get_Sfnt_Table(ft, FT_SFNT_POST);

    FontInfo(&face, &m);
    CHECK(m.fm_UnitsPerEm == ft->units_per_EM && m.fm_NumGlyphs == ft->num_glyphs, "%s: upem %u glyphs %u, FreeType %u %ld",
          font, m.fm_UnitsPerEm, m.fm_NumGlyphs, ft->units_per_EM, ft->num_glyphs);
    CHECK(head && m.fm_XMin == head->xMin && m.fm_YMin == head->yMin && m.fm_XMax == head->xMax &&
              m.fm_YMax == head->yMax, "%s: head's box", font);
    CHECK(hhea && m.fm_Ascender == hhea->Ascender && m.fm_Descender == hhea->Descender &&
              m.fm_LineGap == hhea->Line_Gap, "%s: hhea %d %d %d, FreeType %d %d %d", font, m.fm_Ascender,
          m.fm_Descender, m.fm_LineGap, hhea->Ascender, hhea->Descender, hhea->Line_Gap);
    CHECK(os2 && m.fm_TypoAscender == os2->sTypoAscender && m.fm_TypoDescender == os2->sTypoDescender &&
              m.fm_TypoLineGap == os2->sTypoLineGap && m.fm_WinAscent == os2->usWinAscent &&
              m.fm_WinDescent == os2->usWinDescent && m.fm_Weight == os2->usWeightClass &&
              m.fm_FsSelection == os2->fsSelection &&
              (os2->version < 2 || (m.fm_XHeight == os2->sxHeight && m.fm_CapHeight == os2->sCapHeight)),
          "%s: OS/2", font);
    CHECK(post && m.fm_ItalicAngle == post->italicAngle && m.fm_UnderlinePosition == post->underlinePosition &&
              m.fm_UnderlineThickness == post->underlineThickness && m.fm_FixedPitch == post->isFixedPitch,
          "%s: post", font);
    for (unsigned id = 1; id <= 17; id++)
        name_check(font, ft, &face, id);
    for (uint32_t c = 0; c <= 0x10ffff; c++) {
        uint32_t ours = MapCodepoint(&face, c), theirs = FT_Get_Char_Index(ft, c);

        CHECK(ours == theirs, "%s: U+%04X is glyph %u, FreeType %u", font, c, ours, theirs);
        cmap_checked += theirs != 0;
    }
    for (unsigned g = 0; g < m.fm_NumGlyphs; g++) {
        struct GlyphMetrics gm;
        FT_Fixed adv = 0;

        CHECK(GetGlyphMetrics(&face, g, &gm) == 0, "%s glyph %u: GetGlyphMetrics", font, g);
        FT_Get_Advance(ft, g, FT_LOAD_NO_SCALE, &adv);
        CHECK(gm.gm_Advance == adv, "%s glyph %u: advance %u, FreeType %ld", font, g, gm.gm_Advance, adv);
    }
    if (failures == fails0)
        printf("font-oracle: ok: pass 1: %s: metrics, names, %u advances, %u characters mapped\n", font,
               m.fm_NumGlyphs, cmap_checked);
    fails0 = failures;

    /* Pass 3: points in font units, and at 16 px as FreeType scales them. */
    FT_Set_Pixel_Sizes(ft, 0, 16);
    for (unsigned g = 0; g < m.fm_NumGlyphs; g++) {
        bool comp = is_composite(f, g);

        composites += comp;
        raw.n = raw.ncont = 0;
        e = font_glyph_points(f, g, 0x10000, &raw.s);
        CHECK(e == 0, "%s glyph %u: points %lld", font, g, (long long)e);
        if (e == 0 && FT_Load_Glyph(ft, g, FT_LOAD_NO_SCALE) == 0)
            compare_points(font, g, &ft->glyph->outline, comp ? 1 : 0, "in font units");
        raw.n = raw.ncont = 0;
        e = font_glyph_points(f, g, font_divfix(16 << 6, f->upem), &raw.s);
        if (e == 0 && FT_Load_Glyph(ft, g, FT_LOAD_NO_HINTING) == 0)
            compare_points(font, g, &ft->glyph->outline, 0, "at 16 px");
    }
    CHECK(ft->size->metrics.x_scale == font_divfix(16 << 6, f->upem), "%s: 16 px's scale %ld, ours %lld", font,
          ft->size->metrics.x_scale, (long long)font_divfix(16 << 6, f->upem));
    if (failures == fails0)
        printf("font-oracle: ok: pass 3: %s: %u outlines (%u composite), in font units and at 16 px\n", font,
               m.fm_NumGlyphs, composites);
    fails0 = failures;

    /* Pass 4: coverage at each size. */
    static struct GfxPoint pts[16384];
    static uint8_t verbs[16384];
    static uint8_t mask[512 * 512];
    static int32_t scratch[514 * 512];
    static uint8_t exact[512 * 512];
    struct GfxPath path = { pts, verbs, 16384, 16384, 0, 0 };
    struct stats vs_exact = { 0 }, straight = { 0 }, curved = { 0 };

    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        FT_Set_Pixel_Sizes(ft, 0, sizes[k]);
        for (unsigned g = 0; g < m.fm_NumGlyphs; g++) {
            struct GfxRect r = { 0, 0, -1, -1 };
            struct GfxMask mk = { mask, 0, 0, 0, 0, 0 };
            FT_Bitmap *bm;
            int w = 0, h = 0, ol, ot, fl, ftop;

            e = GlyphOutline(&face, g, sizes[k] << 6, &path);
            CHECK(e == 0, "%s glyph %u at %u px: GlyphOutline %lld", font, g, sizes[k], (long long)e);
            if (e != 0 || FT_Load_Glyph(ft, g, FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL) != 0 ||
                FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_NORMAL) != 0)
                continue;
            bm = &ft->glyph->bitmap;
            bool lines = true;

            for (uint32_t v = 0; v < path.gp_NumVerbs; v++)
                lines &= path.gp_Verbs[v] != PATH_QUAD;
            if (PathBounds(&path, &r)) {
                w = r.MaxX - r.MinX + 1;
                h = r.MaxY - r.MinY + 1;
            }
            CHECK(w <= 512 && h <= 512, "%s glyph %u at %u px: %d x %d", font, g, sizes[k], w, h);
            if (w > 512 || h > 512)
                continue;
            mk = (struct GfxMask){ mask, (uint32_t)w, (uint32_t)h, (uint32_t)w, r.MinX, r.MinY };
            if (w && h) {
                CHECK(PathMask(&path, 0, &mk, scratch, sizeof scratch), "%s glyph %u: PathMask", font, g);
                reference(&path, w, h, r.MinX, r.MinY, exact);
                for (int i = 0; i < w * h; i++)
                    count(&vs_exact, mask[i], exact[i]);
            }
            fl = ft->glyph->bitmap_left;
            ftop = ft->glyph->bitmap_top;
            ol = r.MinX;
            ot = -r.MinY;
            if (bm->width == 0 && w == 0)
                continue;
            CHECK(abs(ol - fl) <= 1 && abs(ot - ftop) <= 1 && abs(w - (int)bm->width) <= 1 &&
                      abs(h - (int)bm->rows) <= 1,
                  "%s glyph %u at %u px: box %d,%d %dx%d, FreeType %d,%d %ux%u", font, g, sizes[k], ol, ot, w, h, fl,
                  ftop, bm->width, bm->rows);
            /* Over both boxes, each pixel as either has it (0 outside). */
            int x0 = ol < fl ? ol : fl, x1 = ol + w > fl + (int)bm->width ? ol + w : fl + (int)bm->width;
            int y0 = ot > ftop ? ot : ftop, y1 = ot - h < ftop - (int)bm->rows ? ot - h : ftop - (int)bm->rows;

            for (int y = y0; y > y1; y--)
                for (int x = x0; x < x1; x++) {
                    int a = 0, b = 0;
                    int ox = x - ol, oy = ot - y, fx = x - fl, fy = ftop - y;

                    if (ox >= 0 && ox < w && oy >= 0 && oy < h)
                        a = mask[oy * w + ox];
                    if (fx >= 0 && fx < (int)bm->width && fy >= 0 && fy < (int)bm->rows)
                        b = bm->buffer[fy * bm->pitch + fx];
                    count(lines ? &straight : &curved, a, b);
                }
            glyphs_drawn++;
        }
    }
    CHECK(vs_exact.worst <= 2 && pct(&vs_exact) >= 99.8,
          "%s: coverage against the exact: worst %d/255, %.3f%% of %lu within 1/255", font, vs_exact.worst,
          pct(&vs_exact), vs_exact.n);
    CHECK(straight.worst <= STRAIGHT_WORST && pct(&straight) >= STRAIGHT_WITHIN1,
          "%s: straight-edged glyphs against FreeType: worst %d/255, %.3f%% of %lu within 1/255", font,
          straight.worst, pct(&straight), straight.n);
    CHECK(curved.worst <= CURVED_WORST && pct(&curved) >= CURVED_WITHIN1,
          "%s: curved glyphs against FreeType: worst %d/255, %.3f%% of %lu within 1/255", font, curved.worst,
          pct(&curved), curved.n);
    if (failures == fails0)
        printf("font-oracle: ok: pass 4: %s at 5 sizes: against the exact, worst %d/255, %.3f%% within 1/255; "
               "against FreeType, straight edges %d/255 and %.3f%%, curves %d/255 and %.3f%%\n", font,
               vs_exact.worst, pct(&vs_exact), straight.worst, pct(&straight), curved.worst, pct(&curved));
    all_exact.n += vs_exact.n, all_exact.in1 += vs_exact.in1;
    all_exact.worst = vs_exact.worst > all_exact.worst ? vs_exact.worst : all_exact.worst;
    all_straight.n += straight.n, all_straight.in1 += straight.in1;
    all_straight.worst = straight.worst > all_straight.worst ? straight.worst : all_straight.worst;
    all_curved.n += curved.n, all_curved.in1 += curved.in1;
    all_curved.worst = curved.worst > all_curved.worst ? curved.worst : all_curved.worst;
    shaping(font, buf, len, &face);
    FT_Done_Face(ft);
    free(buf);
}

/* --- Malformed fonts, made by hand from a good one --- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

/* Every glyph drawn, whatever it says: no fault, and an answer. */
static void draw_all(const struct FontFace *face)
{
    static struct GfxPoint pts[4096];
    static uint8_t verbs[4096];
    struct GfxPath path = { pts, verbs, 4096, 4096, 0, 0 };
    struct FontMetrics m;

    FontInfo(face, &m);
    for (unsigned g = 0; g < m.fm_NumGlyphs; g++) {
        int64_t e = GlyphOutline(face, g, 16 << 6, &path);

        CHECK(e == 0 || e == ERR_BADFONT || e == ERR_UNSUPPORTED || e == ERR_FULL, "glyph %u: %lld", g, (long long)e);
    }
    for (uint32_t c = 0; c < 0x300; c++)
        MapCodepoint(face, c);
}

static void malformed(const char *path)
{
    size_t len;
    uint8_t *good = load(path, &len);
    struct FontFace face;
    unsigned fails0 = failures, cut = 0, opened = 0;

    /* Cut short at every length (in steps, through the whole file). */
    for (size_t n = 0; n < len; n += n < 4096 ? 1 : 61) {
        uint8_t *b = malloc(n ? n : 1);

        memcpy(b, good, n);
        if (OpenFont(&face, (struct Span){ b, n }) == 0) {
            opened++;
            draw_all(&face);
        }
        cut++;
        free(b);
    }

    /* A composite that holds itself; a glyph past glyf's end. */
    uint8_t *b = malloc(len);

    memcpy(b, good, len);
    CHECK(OpenFont(&face, (struct Span){ b, len }) == 0, "malformed: the good one won't open");
    const struct font *f = font_of(&face);
    int self = -1;

    for (unsigned g = 0; g < f->nglyphs && self < 0; g++)
        if (is_composite(f, g))
            self = (int)g;
    CHECK(self >= 0, "malformed: no composite to break");
    if (self >= 0) {
        uint32_t from;
        uint16_t a;
        uint8_t gpath[64];
        struct GfxPoint pts[64];
        struct GfxPath p = { pts, gpath, 64, 64, 0, 0 };

        if (f->loca_long)
            SpanReadU32BE(f->loca, 4 * (uint64_t)self, &from);
        else
            SpanReadU16BE(f->loca, 2 * (uint64_t)self, &a), from = 2u * a;
        put16((uint8_t *)f->glyf.sp_Data + from + 12, (uint16_t)self);   /* its first component: itself */
        CHECK(GlyphOutline(&face, (unsigned)self, 16 << 6, &p) == ERR_BADFONT, "malformed: a composite of itself");
        if (f->loca_long) {
            uint8_t *l = (uint8_t *)f->loca.sp_Data + 4 * (uint64_t)self + 4;

            l[0] = 0x7f;
        } else {
            put16((uint8_t *)f->loca.sp_Data + 2 * (uint64_t)self + 2, 0xffff);
        }
        CHECK(GlyphOutline(&face, (unsigned)self, 16 << 6, &p) == ERR_BADFONT, "malformed: a glyph past glyf");
    }

    /* The file's kind, and head's magic. */
    memcpy(b, good, len);
    memcpy(b, "OTTO", 4);
    CHECK(OpenFont(&face, (struct Span){ b, len }) == ERR_UNSUPPORTED, "malformed: CFF");
    memcpy(b, "ttcf", 4);
    CHECK(OpenFont(&face, (struct Span){ b, len }) == ERR_UNSUPPORTED, "malformed: a collection");
    memcpy(b, good, len);
    OpenFont(&face, (struct Span){ b, len });
    ((uint8_t *)font_of(&face)->head.sp_Data)[12] ^= 0xff;
    CHECK(OpenFont(&face, (struct Span){ b, len }) == ERR_BADFONT, "malformed: head's magic");
    free(b);
    free(good);
    if (failures == fails0)
        printf("font-oracle: ok: malformed: %s cut short %u ways (%u still opened, every glyph drawn), a composite of "
               "itself, a glyph past glyf, CFF, a collection, a bad magic\n", base(path), cut, opened);
}

int main(int argc, char **argv)
{
    FT_Library lib;

    if (argc < 2) {
        fprintf(stderr, "usage: font-oracle FONT.ttf ...\n");
        return 2;
    }
    if (FT_Init_FreeType(&lib))
        return 2;
    for (int i = 1; i < argc; i++)
        one_font(lib, argv[i]);
    malformed(argv[argc - 1]);
    shaping_alone();

    /* conformance/fontcheck.h's CRCs, which fonttest checks on QEMU. */
    static const struct fontcheck_calls calls = { SystemFont, OpenFont, MapCodepoint, GlyphOutline, PathBounds,
                                                  PathMask };
    unsigned fails0 = failures;

    for (unsigned i = 0; i < sizeof fontcheck_fonts / sizeof fontcheck_fonts[0]; i++) {
        uint32_t crc = fontcheck_run(&calls, fontcheck_fonts[i]);

        CHECK(crc && crc == fontcheck_crcs[i], "fontcheck.h: font %#x's CRC is %#010x, not %#010x", fontcheck_fonts[i],
              crc, fontcheck_crcs[i]);
    }
    static const struct shapecheck_calls shape_calls = { SystemFont, OpenFont, ShapeRun, FitLine, LineSpacing };

    for (unsigned i = 0; i < sizeof fontcheck_fonts / sizeof fontcheck_fonts[0]; i++) {
        uint32_t crc = shapecheck_run(&shape_calls, fontcheck_fonts[i]);

        CHECK(crc && crc == shapecheck_crcs[i], "fontcheck.h: font %#x's shaping CRC is %#010x, not %#010x",
              fontcheck_fonts[i], crc, shapecheck_crcs[i]);
    }
    if (failures == fails0)
        printf("font-oracle: ok: fontcheck.h's CRCs, the system fonts' glyphs at 13 px and their text shaped\n");
    FT_Done_FreeType(lib);
    if (failures) {
        fprintf(stderr, "font-oracle: %u checks failed\n", failures);
        return 1;
    }
    printf("font-oracle: ok: %d fonts, %lu glyphs drawn: against the exact, worst %d/255, %.3f%% of %lu pixels "
           "within 1/255\n", argc - 1, glyphs_drawn, all_exact.worst, pct(&all_exact), all_exact.n);
    return 0;
}
