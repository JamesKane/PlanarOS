/*
 * conformance/typecheck.h -- gfx.library's paths and type (V8 step 3)
 * against a reference, to the bit
 *
 * On gfxcheck.h's two bitmaps and their guards, through the interface
 * under test:
 *
 * - BltMask at every width from 0 to 130, 8 alignments and gfxcheck.h's
 *   five places, coverage in runs of clear, covered and between, against
 *   tc_mask_px, written apart from rows.h's gfx_mask_px.
 * - PathMask (the sweep) against lib/gfx's scalar PathMask on random
 *   paths, every width from 1 to 70, both fill rules; and a mask cut
 *   from a bigger one the same bytes as the bigger one's.
 * - FillPath and FillGradient against one whole mask of the scalar
 *   PathMask's, blended by tc_mask_px: so FillPath's pieces join without
 *   a seam. Gradients against tc_ramp, each way, over a rectangle and a
 *   path. PathRoundRect's corners and refusals.
 * - Text and TextRun in System Sans and System Mono, by the interface
 *   and by lib/gfx's scalar entries, each with its own font and cache,
 *   in JAM1 and JAM2, clipped; and TextLength.
 *
 * Then typescene.h's scene, a little of each: its CRC is the host's
 * (tc_scene_crc), on either ISA, scalar or by the vector unit.
 * Shared by lib/gfx/test.c on the host, and conformance/typetest.c in
 * the guest, which shows the scene in a window too (conformance/window.c).
 */
#ifndef CONFORMANCE_TYPECHECK_H
#define CONFORMANCE_TYPECHECK_H

#include <planar/font.h>

#include "../lib/gfx/rows.h"            /* gfx_linear and gfx_srgb, the tables */
#include "gfxcheck.h"
#include "typescene.h"

/* The scene's CRC-32 (typescene.h), as lib/gfx/test.c prints it. */
constexpr uint32_t tc_scene_crc = 0x880b8f12;

/* BltMask's pixel, as gfx.h says it: the weight c out of 256, 255's
 * 256; linear light by the tables, alpha as it is. */
static uint32_t tc_mask_px(uint32_t s, uint32_t d, uint32_t c)
{
    uint32_t w = c >= 128 ? c + 1 : c, r = 0;

    if (c == 0 || c == 255)
        return c ? s : d;
    for (unsigned k = 0; k < 4; k++) {
        uint32_t sc = s >> 8 * k & 0xff, dc = d >> 8 * k & 0xff, v;

        if (k == 3)
            v = (sc * w + dc * (256 - w) + 128) / 256;
        else
            v = gfx_srgb[(gfx_linear[sc] * w + gfx_linear[dc] * (256 - w) + 128) / 256 / 16];
        r |= v << 8 * k;
    }
    return r;
}

/* One channel at a time, from 'from' at lo to 'to' at hi, to the
 * nearest, a half up. */
static uint32_t tc_ramp(uint32_t from, uint32_t to, int64_t lo, int64_t hi, int64_t p)
{
    uint32_t r = 0;

    if (hi <= lo || p <= lo)
        return from;
    if (p >= hi)
        return to;
    for (unsigned k = 0; k < 4; k++) {
        int64_t f = from >> 8 * k & 0xff, t = to >> 8 * k & 0xff;

        r |= (uint32_t)((2 * (f * (hi - p) + t * (p - lo)) + (hi - lo)) / (2 * (hi - lo))) << 8 * k;
    }
    return r;
}

static uint32_t tc_colour(const struct GfxGradient *g, uint32_t pen, int64_t x, int64_t y)
{
    if (!g)
        return pen;
    if (g->gg_Flags & GRADF_HORIZONTAL)
        return tc_ramp(g->gg_From, g->gg_To, g->gg_Rect.MinX, g->gg_Rect.MaxX, x);
    return tc_ramp(g->gg_From, g->gg_To, g->gg_Rect.MinY, g->gg_Rect.MaxY, y);
}

/* --- Paths. --- */

constexpr unsigned TC_MAXPTS = 64;
static struct GfxPoint tc_pts[TC_MAXPTS];
static uint8_t tc_verbs[TC_MAXPTS];
static struct GfxPath tc_path = { tc_pts, tc_verbs, TC_MAXPTS, TC_MAXPTS, 0, 0 };

/* A random path about the box (x0, y0) to (x1, y1), in pixels. */
static void tc_random_path(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    unsigned verbs = 3 + gc_rand() % 10;

#define TC_PT() \
    (tc_pts[tc_path.gp_NumPoints++] = (struct GfxPoint){ \
         (int32_t)((x0 - 8) * 64 + (int32_t)(gc_rand() % (uint32_t)((x1 - x0 + 16) * 64))), \
         (int32_t)((y0 - 8) * 64 + (int32_t)(gc_rand() % (uint32_t)((y1 - y0 + 16) * 64))) })
    tc_path.gp_NumPoints = tc_path.gp_NumVerbs = 0;
    tc_verbs[tc_path.gp_NumVerbs++] = PATH_MOVE;
    TC_PT();
    for (unsigned i = 0; i < verbs; i++) {
        uint32_t v = gc_rand() % 16;
        uint8_t verb = v < 6 ? PATH_LINE : v < 10 ? PATH_QUAD : v < 14 ? PATH_CUBIC : v < 15 ? PATH_CLOSE : PATH_MOVE;
        unsigned n = verb == PATH_LINE || verb == PATH_MOVE ? 1 : verb == PATH_QUAD ? 2 : verb == PATH_CUBIC ? 3 : 0;

        tc_verbs[tc_path.gp_NumVerbs++] = verb;
        for (unsigned k = 0; k < n; k++)
            TC_PT();
    }
#undef TC_PT
}

static uint8_t tc_cov[2][200 * 40];
static int32_t tc_scratch[(200 + 2) * 40];

static unsigned tc_sweep(const struct GfxIFace *g, unsigned *cases)
{
    for (unsigned t = 0; t < 120; t++) {
        tc_random_path(0, 0, 80, 24);
        for (uint32_t w = 1; w <= 70; w++) {
            uint32_t flags = (t + w) & 1 ? PATHF_EVENODD : 0;
            struct GfxMask m = { tc_cov[0], w, 1 + w % 23, 80, (int32_t)(t % 7) - 3, (int32_t)(w % 5) - 2 };
            struct GfxMask r = m;

            r.gm_Pixels = tc_cov[1];
            gc_w = (int)w, gc_a = (int)t, gc_p = 100;
            for (unsigned i = 0; i < sizeof tc_cov[0]; i++)
                tc_cov[0][i] = tc_cov[1][i] = (uint8_t)i;
            GC(g->PathMask(&tc_path, flags, &m, tc_scratch, sizeof tc_scratch));
            GC(PathMask(&tc_path, flags, &r, tc_scratch, sizeof tc_scratch));
            for (unsigned i = 0; i < sizeof tc_cov[0]; i++)
                GC(tc_cov[0][i] == tc_cov[1][i]);
        }
        /* A piece of a bigger mask: the same bytes. */
        struct GfxMask big = { tc_cov[0], 96, 32, 96, -8, -4 };
        GC(PathMask(&tc_path, t & 1 ? PATHF_EVENODD : 0, &big, tc_scratch, sizeof tc_scratch));
        for (unsigned k = 0; k < 8; k++) {
            uint32_t px = gc_rand() % 90, py = gc_rand() % 28, w = 1 + gc_rand() % (96 - px), h = 1 + gc_rand() % (32 - py);
            struct GfxMask part = { tc_cov[1], w, h, w, big.gm_X + (int32_t)px, big.gm_Y + (int32_t)py };

            gc_w = (int)w, gc_a = (int)t, gc_p = 101;
            GC(g->PathMask(&tc_path, t & 1 ? PATHF_EVENODD : 0, &part, tc_scratch, sizeof tc_scratch));
            for (uint32_t j = 0; j < h; j++)
                for (uint32_t i = 0; i < w; i++)
                    GC(tc_cov[1][j * w + i] == tc_cov[0][(py + j) * 96 + px + i]);
        }
    }
    /* Refused: verbs and points that don't agree; short scratch. */
    tc_path.gp_NumPoints--;
    struct GfxMask m = { tc_cov[0], 10, 10, 10, 0, 0 };
    GC(!g->PathMask(&tc_path, 0, &m, tc_scratch, sizeof tc_scratch));
    tc_path.gp_NumPoints++;
    GC(!g->PathMask(&tc_path, 0, &m, tc_scratch, PathMaskScratch(10, 10) - 1));
    return 0;
}

/* --- Masks. --- */

static unsigned tc_masks(const struct GfxIFace *g, unsigned *cases)
{
    for (int32_t w = 0; w <= 130; w++)
        for (int32_t a = 0; a < 8; a++)
            for (int p = 0; p < 5; p++) {
                int32_t r[4], h = GC_H + 6;
                struct GfxMask m = { tc_cov[0], (uint32_t)w, (uint32_t)h, (uint32_t)w + 3,
                                     (int32_t)(gc_rand() % 5) - 2, (int32_t)(gc_rand() % 5) - 2 };
                uint32_t pen = gc_rand();

                gc_w = w, gc_a = a, gc_p = 200 + p;
                gc_reset();
                gc_place(w, a, p, r);
                /* Runs: clear, covered, and between. */
                for (unsigned i = 0; i < sizeof tc_cov[0];) {
                    uint32_t kind = gc_rand() % 3, run = 1 + gc_rand() % 40;

                    for (; run && i < sizeof tc_cov[0]; run--, i++)
                        tc_cov[0][i] = kind == 0 ? 0 : kind == 1 ? 255 : (uint8_t)gc_rand();
                }
                gc_rp[0].rp_FgPen = gc_rp[1].rp_FgPen = pen;
                g->BltMask(&gc_rp[0], &m, r[0] - m.gm_X, r[1] - m.gm_Y);
                for (int32_t j = 0; j < h; j++)
                    for (int32_t i = 0; i < w; i++) {
                        int64_t x = r[0] + i, y = r[1] + j;

                        if (gr_in(&gc_rp[1], x, y))
                            *gr_px(&gc_bm[1], x, y) =
                                tc_mask_px(pen, *gr_px(&gc_bm[1], x, y), tc_cov[0][j * (w + 3) + i]);
                    }
                GC(gc_same());
            }
    /* No mask, or rows shorter than it's wide: nothing. */
    gc_reset();
    g->BltMask(&gc_rp[0], nullptr, 0, 0);
    struct GfxMask bad = { tc_cov[0], 10, 2, 9, 0, 0 };
    g->BltMask(&gc_rp[0], &bad, 0, 0);
    GC(gc_same());
    return 0;
}

/* The path, or the gradient's rectangle, drawn by the reference: one
 * whole mask over its box within the clip. */
static bool tc_ref_fill(struct RastPort *rp, const struct GfxPath *path, uint32_t flags,
                        const struct GfxGradient *g)
{
    int64_t cx0, cy0, cx1, cy1;
    struct GfxRect b;

    cx0 = rp->rp_Clip.MinX > 0 ? rp->rp_Clip.MinX : 0;
    cy0 = rp->rp_Clip.MinY > 0 ? rp->rp_Clip.MinY : 0;
    cx1 = rp->rp_Clip.MaxX < (int64_t)rp->rp_BitMap->bm_Width - 1 ? rp->rp_Clip.MaxX
                                                                   : (int64_t)rp->rp_BitMap->bm_Width - 1;
    cy1 = rp->rp_Clip.MaxY < (int64_t)rp->rp_BitMap->bm_Height - 1 ? rp->rp_Clip.MaxY
                                                                    : (int64_t)rp->rp_BitMap->bm_Height - 1;
    if (!path) {
        b = g->gg_Rect;
    } else if (!PathBounds(path, &b)) {
        return true;
    }
    cx0 = b.MinX > cx0 ? b.MinX : cx0;
    cy0 = b.MinY > cy0 ? b.MinY : cy0;
    cx1 = b.MaxX < cx1 ? b.MaxX : cx1;
    cy1 = b.MaxY < cy1 ? b.MaxY : cy1;
    if (cx0 > cx1 || cy0 > cy1)
        return true;
    uint32_t w = (uint32_t)(cx1 - cx0 + 1), h = (uint32_t)(cy1 - cy0 + 1);
    struct GfxMask m = { tc_cov[1], w, h, w, (int32_t)cx0, (int32_t)cy0 };

    if (w * h > sizeof tc_cov[1])
        return false;
    if (path && !PathMask(path, flags, &m, tc_scratch, sizeof tc_scratch))
        return false;
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++) {
            uint32_t *d = gr_px(rp->rp_BitMap, cx0 + i, cy0 + j);
            uint32_t s = tc_colour(g, rp->rp_FgPen, cx0 + i, cy0 + j);

            *d = path ? tc_mask_px(s, *d, m.gm_Pixels[j * w + i]) : s;
        }
    return true;
}

static uint8_t tc_tmpras[4096];

static unsigned tc_fills(const struct GfxIFace *g, unsigned *cases)
{
    struct TmpRas tr;

    g->InitTmpRas(&tr, tc_tmpras, sizeof tc_tmpras);
    GC(tr.tr_RasPtr == tc_tmpras && tr.tr_Size == sizeof tc_tmpras);
    for (unsigned t = 0; t < 600; t++) {
        uint32_t flags = t & 1 ? PATHF_EVENODD : 0, pen = gc_rand();
        struct GfxGradient gr = {
            { (int32_t)(gc_rand() % 160) - 10, (int32_t)(gc_rand() % 14) - 2, 0, 0 },
            gc_rand(), gc_rand(), t & 2 ? GRADF_HORIZONTAL : 0,
        };

        gr.gg_Rect.MaxX = gr.gg_Rect.MinX + (int32_t)(gc_rand() % 100) - 5;
        gr.gg_Rect.MaxY = gr.gg_Rect.MinY + (int32_t)(gc_rand() % 12) - 1;
        tc_random_path(-10, -4, GC_W + 10, GC_H + 4);
        gc_w = (int)t, gc_a = 0, gc_p = 300;
        gc_reset();
        for (unsigned k = 0; k < 2; k++) {
            gc_rp[k].rp_FgPen = pen;
            gc_rp[k].rp_TmpRas = t % 3 == 0 ? &tr : nullptr;
            if (t % 5 == 4)
                gc_rp[k].rp_Clip = (struct GfxRect){ 3, 2, GC_W - 20, GC_H - 3 };
        }
        g->FillPath(&gc_rp[0], &tc_path, flags);
        GC(tc_ref_fill(&gc_rp[1], &tc_path, flags, nullptr));
        GC(gc_same());
        gc_a = 1;
        g->FillGradient(&gc_rp[0], &gr, nullptr, 0);
        GC(tc_ref_fill(&gc_rp[1], nullptr, 0, &gr));
        GC(gc_same());
        gc_a = 2;
        g->FillGradient(&gc_rp[0], &gr, &tc_path, flags);
        GC(tc_ref_fill(&gc_rp[1], &tc_path, flags, &gr));
        GC(gc_same());
        gc_rp[0].rp_TmpRas = gc_rp[1].rp_TmpRas = nullptr;
    }
    /* A square corner is RectFill's; a rounded one leaves the corner. */
    struct GfxRect box = { 5, 1, 60, 8 };
    gc_reset();
    tc_path.gp_NumPoints = tc_path.gp_NumVerbs = 0;
    GC(g->PathRoundRect(&tc_path, &box, 0) && tc_path.gp_NumVerbs == 5 && tc_path.gp_NumPoints == 4);
    gc_rp[0].rp_FgPen = gc_rp[1].rp_FgPen = 0xff336699u;
    g->FillPath(&gc_rp[0], &tc_path, 0);
    g->RectFill(&gc_rp[1], 5, 1, 60, 8);
    GC(gc_same());
    tc_path.gp_NumPoints = tc_path.gp_NumVerbs = 0;
    GC(g->PathRoundRect(&tc_path, &box, 3 * 64) && tc_path.gp_NumVerbs == 10 && tc_path.gp_NumPoints == 17);
    gc_reset();
    g->FillPath(&gc_rp[0], &tc_path, 0);
    GC(*gr_px(&gc_bm[0], 5, 1) != 0xff336699u && *gr_px(&gc_bm[0], 30, 4) == 0xff336699u);
    /* A ring: two subpaths, even-odd. */
    struct GfxRect inner = { 10, 3, 55, 6 };
    GC(g->PathRoundRect(&tc_path, &inner, 64) && tc_path.gp_NumVerbs == 20);
    gc_reset();
    gc_rp[0].rp_FgPen = gc_rp[1].rp_FgPen = 0xff336699u;
    g->FillPath(&gc_rp[0], &tc_path, PATHF_EVENODD);
    GC(tc_ref_fill(&gc_rp[1], &tc_path, PATHF_EVENODD, nullptr));
    GC(gc_same() && *gr_px(&gc_bm[0], 30, 4) != 0xff336699u);
    /* Full arrays: refused, the path as it was; an empty rectangle adds nothing. */
    tc_path.gp_MaxVerbs = tc_path.gp_NumVerbs + 9;
    GC(!g->PathRoundRect(&tc_path, &box, 64) && tc_path.gp_NumVerbs == 20);
    GC(g->PathRoundRect(&tc_path, &(struct GfxRect){ 5, 5, 4, 9 }, 64) && tc_path.gp_NumVerbs == 20);
    tc_path.gp_MaxVerbs = TC_MAXPTS;
    return 0;
}

/* --- Type. --- */

static struct ShapedGlyph tc_glyphs[96];

/* font.library's table, to shape TextRun's run: the caller's to set. */
static const struct FontIFace *tc_ifont;

static unsigned tc_type(const struct GfxIFace *g, const struct GfxIFace *ref, unsigned *cases)
{
    static const char *const texts[] = {
        "Planar 8, type at last", "AVATAR Wolf ffi \xc3\xa9t\xc3\xa9 1,234.50",
        "\t\xc2\xad\xe2\x80\x94 \x01x", "", "WWWWWWWWWWWWWWWWWWWWWWW",
    };
    static const char *const names[] = { nullptr, "System Mono", "System Sans" };
    static const uint16_t sizes[] = { 13, 9, 13 };
    static const uint8_t styles[] = { 0, 0, FSF_BOLD };
    static const int32_t xs[] = { 0, 3, -7, GC_W - 30 }, ys[] = { -2, 0, -6 };

    for (unsigned f = 0; f < 3; f++) {
        struct TextFont *a = tc_open(g, names[f], sizes[f], styles[f]);
        struct TextFont *b = tc_open(ref, names[f], sizes[f], styles[f]);

        gc_w = (int)f, gc_a = 0, gc_p = 400;
        GC(a && b && (a->tf_Flags & FPF_OUTLINE) && a->tf_YSize == b->tf_YSize && !a->tf_CharData);
        GC(a->tf_YSize >= sizes[f] && a->tf_Baseline > 0 && a->tf_Baseline < a->tf_YSize);
        GC(f == 1 ? (a->tf_Flags & FPF_FIXED) && a->tf_XSize > 0 : a->tf_XSize == 0);
        for (unsigned t = 0; t < sizeof texts / sizeof texts[0]; t++)
            for (unsigned xi = 0; xi < sizeof xs / sizeof xs[0]; xi++)
                for (unsigned yi = 0; yi < sizeof ys / sizeof ys[0]; yi++)
                    for (uint32_t mode = JAM1; mode <= JAM2; mode++) {
                        uint64_t n = 0;

                        while (texts[t][n])
                            n++;
                        gc_w = (int)f, gc_a = (int)t, gc_p = 410 + (int)(xi * 10 + yi);
                        gc_reset();
                        for (unsigned k = 0; k < 2; k++) {
                            gc_rp[k].rp_FgPen = 0xffe8eef6u, gc_rp[k].rp_BgPen = 0xff36435au;
                            gc_rp[k].rp_DrawMode = mode;
                        }
                        g->SetFont(&gc_rp[0], a);
                        ref->SetFont(&gc_rp[1], b);
                        GC(gc_rp[0].rp_Font == a);
                        g->Text(&gc_rp[0], xs[xi], ys[yi], texts[t], n);
                        ref->Text(&gc_rp[1], xs[xi], ys[yi], texts[t], n);
                        GC(gc_same());
                        GC(g->TextLength(&gc_rp[0], texts[t], n) == ref->TextLength(&gc_rp[1], texts[t], n));
                    }
        /* A run, in part: shaped once, drawn by both. */
        struct FontFace face;
        struct Span file;
        struct GlyphRun run = { tc_glyphs, 96, 0, 0, 0 };
        uint64_t len = 0;

        while (texts[1][len])
            len++;
        gc_reset();
        gc_rp[0].rp_FgPen = gc_rp[1].rp_FgPen = 0xff102030u;
        gc_rp[0].rp_DrawMode = gc_rp[1].rp_DrawMode = JAM1;
        if (tc_ifont && tc_ifont->SystemFont((f == 1 ? FONT_MONO : FONT_SANS) | (styles[f] ? FONT_BOLD : 0), &file) &&
            tc_ifont->OpenFont(&face, file) == 0 &&
            tc_ifont->ShapeRun(&face, texts[1], len, FONT_FEAT_KERN | FONT_FEAT_LIGA, &run) == 0) {
            g->TextRun(&gc_rp[0], &run, 2, 20, 4, 9);
            ref->TextRun(&gc_rp[1], &run, 2, 20, 4, 9);
            g->TextRun(&gc_rp[0], &run, 30, 50, 70, 5);         /* to its end */
            ref->TextRun(&gc_rp[1], &run, 30, 50, 70, 5);
            g->TextRun(&gc_rp[0], &run, 99, 1, 0, 5);          /* none */
            GC(gc_same() && run.gr_Count > 20);
        } else {
            GC(false);
        }
        /* Clipped; then the 8×8 again, by SetFont(nullptr). */
        gc_reset();
        for (unsigned k = 0; k < 2; k++)
            gc_rp[k].rp_Clip = (struct GfxRect){ 7, 2, GC_W - 40, GC_H - 4 };
        g->Text(&gc_rp[0], 2, -1, texts[0], 22);
        ref->Text(&gc_rp[1], 2, -1, texts[0], 22);
        GC(gc_same());
        g->SetFont(&gc_rp[0], nullptr);
        ref->SetFont(&gc_rp[1], nullptr);
        GC(g->TextLength(&gc_rp[0], "abc", 3) == 24);
        g->Text(&gc_rp[0], 2, 1, texts[0], 22);
        gr_text(&gc_rp[1], 2, 1, texts[0], 22);
        GC(gc_same());
        g->CloseTextFont(a);
        ref->CloseTextFont(b);
    }
    /* Refused: no such font, no size. */
    GC(!tc_open(g, "Topaz", 13, 0) && !tc_open(g, nullptr, 0, 0) && !tc_open(g, nullptr, 256, 0));
    g->CloseTextFont(nullptr);
    return 0;
}

static uint32_t tc_crc(const struct BitMap *bm)
{
    uint32_t crc = ~0u;

    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            uint32_t px = *gr_px(bm, x, y) & 0xffffff;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

/* Every case through 'g', its text against 'ref''s: 0, or the failed check's line. */
static unsigned tc_all(const struct GfxIFace *g, const struct GfxIFace *ref, unsigned *cases)
{
    unsigned r;

    gc_setup();
    if ((r = tc_masks(g, cases)) || (r = tc_sweep(g, cases)) || (r = tc_fills(g, cases)) ||
        (r = tc_type(g, ref, cases)))
        return r;
    gc_reset();
    return 0;
}

#endif /* CONFORMANCE_TYPECHECK_H */
