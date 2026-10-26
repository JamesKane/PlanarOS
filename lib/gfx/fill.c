/*
 * lib/gfx/fill.c -- BltMask, FillPath, FillGradient, PathRoundRect and
 * InitTmpRas (V8 step 3)
 *
 * Coverage blended in a colour: BltMask's from the caller's mask,
 * FillPath's from PathMask's. A path is drawn a piece at a time: as
 * many rows of its box, as wide as fit, as the scratch holds, which is
 * the rastport's TmpRas, or else a little of the stack, which a 16 KiB
 * stack can spare. Each piece walks the whole path again, so a big path
 * drawn without a TmpRas costs more, but draws the same pixels. A
 * gradient's colours are worked out a row at a time, here, and blended
 * or copied by the kernels like any other row.
 */
#include "rows.h"

constexpr uint64_t STACK_SCRATCH = 2560;    /* bytes: 64 pixels by 7 rows */
constexpr uint32_t ROW = 64;                /* a row's colours, worked out this many at a time */

/* One channel of the gradient at p, from 'from' at lo to 'to' at hi. */
static uint32_t ramp(uint32_t from, uint32_t to, int64_t lo, int64_t hi, int64_t p)
{
    uint32_t r = 0;

    if (p <= lo || hi <= lo)
        return from;
    if (p >= hi)
        return to;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        int64_t f = from >> sh & 0xff, t = to >> sh & 0xff;

        r |= (uint32_t)((f * (hi - p) + t * (p - lo) + (hi - lo) / 2) / (hi - lo)) << sh;
    }
    return r;
}

/* The colours of pixels x to x + n - 1 of row y: the pen's, or the gradient's. */
static void colours(const struct GfxGradient *g, uint32_t pen, int64_t x, int64_t y, uint32_t n,
                    uint32_t *out)
{
    if (g && (g->gg_Flags & GRADF_HORIZONTAL)) {
        for (uint32_t i = 0; i < n; i++)
            out[i] = ramp(g->gg_From, g->gg_To, g->gg_Rect.MinX, g->gg_Rect.MaxX, x + i);
        return;
    }
    if (g)
        pen = ramp(g->gg_From, g->gg_To, g->gg_Rect.MinY, g->gg_Rect.MaxY, y);
    for (uint32_t i = 0; i < n; i++)
        out[i] = pen;
}

/* n pixels of row y from x, through n bytes of coverage. */
static void blend_row(const struct gfx_rows *k, struct RastPort *rp, const struct GfxGradient *g,
                      int64_t x, int64_t y, const uint8_t *cov, uint64_t n)
{
    uint32_t src[ROW];

    while (n) {
        uint32_t piece = n < ROW ? (uint32_t)n : ROW;

        colours(g, rp->rp_FgPen, x, y, piece, src);
        k->mask(gfx_pixel_at(rp->rp_BitMap, x, y), cov, src, piece);
        x += piece;
        cov += piece;
        n -= piece;
    }
}

void gfx_blt_mask(const struct gfx_rows *k, struct RastPort *rp, const struct GfxMask *mask,
                  int32_t x, int32_t y)
{
    int64_t cx0, cy0, cx1, cy1, x0, y0, x1, y1;

    if (!mask || !mask->gm_Pixels || mask->gm_BytesPerRow < mask->gm_Width ||
        !gfx_clip_of(rp, &cx0, &cy0, &cx1, &cy1))
        return;
    x0 = (int64_t)x + mask->gm_X;
    y0 = (int64_t)y + mask->gm_Y;
    x1 = x0 + mask->gm_Width - 1;
    y1 = y0 + mask->gm_Height - 1;
    x0 = x0 > cx0 ? x0 : cx0;
    y0 = y0 > cy0 ? y0 : cy0;
    x1 = x1 < cx1 ? x1 : cx1;
    y1 = y1 < cy1 ? y1 : cy1;
    for (int64_t py = y0; x0 <= x1 && py <= y1; py++) {
        int64_t i = x0 - x - mask->gm_X, j = py - y - mask->gm_Y;

        blend_row(k, rp, nullptr, x0, py, mask->gm_Pixels + j * mask->gm_BytesPerRow + i,
                  (uint64_t)(x1 - x0 + 1));
    }
}

void gfx_fill_path(const struct gfx_rows *k, struct RastPort *rp, const struct GfxPath *path,
                   uint32_t flags, const struct GfxGradient *g)
{
    uint64_t stack[STACK_SCRATCH / 8];
    uint8_t *scratch = (uint8_t *)stack;
    uint64_t size = sizeof stack, per_row;
    int64_t cx0, cy0, cx1, cy1, tw, th;
    struct GfxRect b;

    if (!path || !PathBounds(path, &b) || !gfx_clip_of(rp, &cx0, &cy0, &cx1, &cy1))
        return;
    cx0 = b.MinX > cx0 ? b.MinX : cx0;
    cy0 = b.MinY > cy0 ? b.MinY : cy0;
    cx1 = b.MaxX < cx1 ? b.MaxX : cx1;
    cy1 = b.MaxY < cy1 ? b.MaxY : cy1;
    if (cx0 > cx1 || cy0 > cy1)
        return;
    if (rp->rp_TmpRas && rp->rp_TmpRas->tr_RasPtr) {
        uintptr_t p = (uintptr_t)rp->rp_TmpRas->tr_RasPtr, a = (p + 7) & ~(uintptr_t)7;

        if (rp->rp_TmpRas->tr_Size > a - p + STACK_SCRATCH) {
            scratch = (uint8_t *)a;
            size = rp->rp_TmpRas->tr_Size - (a - p);
        }
    }
    /* A piece's areas, (tw + 2) a row, then its coverage, tw a row. */
    tw = cx1 - cx0 + 1;
    if ((uint64_t)tw * 5 + 8 > size)
        tw = (int64_t)(size - 8) / 5;
    per_row = (uint64_t)(tw + 2) * 4 + (uint64_t)tw;
    th = (int64_t)(size / per_row);
    for (int64_t ty = cy0; ty <= cy1; ty += th)
        for (int64_t tx = cx0; tx <= cx1; tx += tw) {
            uint32_t w = (uint32_t)(cx1 - tx + 1 < tw ? cx1 - tx + 1 : tw);
            uint32_t h = (uint32_t)(cy1 - ty + 1 < th ? cy1 - ty + 1 : th);
            uint64_t areas = PathMaskScratch(w, h);
            struct GfxMask m = {
                .gm_Pixels = scratch + areas, .gm_Width = w, .gm_Height = h, .gm_BytesPerRow = w,
                .gm_X = (int32_t)tx, .gm_Y = (int32_t)ty,
            };

            if (!gfx_path_mask(k, path, flags, &m, scratch, areas))
                return;
            for (uint32_t j = 0; j < h; j++)
                blend_row(k, rp, g, tx, ty + j, m.gm_Pixels + (uint64_t)j * w, w);
        }
}

void gfx_fill_gradient(const struct gfx_rows *k, struct RastPort *rp, const struct GfxGradient *g,
                       const struct GfxPath *path, uint32_t flags)
{
    int64_t cx0, cy0, cx1, cy1;
    uint32_t row[ROW];

    if (!g)
        return;
    if (path) {
        gfx_fill_path(k, rp, path, flags, g);
        return;
    }
    if (!gfx_clip_of(rp, &cx0, &cy0, &cx1, &cy1))
        return;
    cx0 = g->gg_Rect.MinX > cx0 ? g->gg_Rect.MinX : cx0;
    cy0 = g->gg_Rect.MinY > cy0 ? g->gg_Rect.MinY : cy0;
    cx1 = g->gg_Rect.MaxX < cx1 ? g->gg_Rect.MaxX : cx1;
    cy1 = g->gg_Rect.MaxY < cy1 ? g->gg_Rect.MaxY : cy1;
    for (int64_t y = cy0; cx0 <= cx1 && y <= cy1; y++) {
        if (!(g->gg_Flags & GRADF_HORIZONTAL)) {
            k->fill(gfx_pixel_at(rp->rp_BitMap, cx0, y),
                    ramp(g->gg_From, g->gg_To, g->gg_Rect.MinY, g->gg_Rect.MaxY, y),
                    (uint64_t)(cx1 - cx0 + 1));
            continue;
        }
        for (int64_t x = cx0; x <= cx1; x += ROW) {
            uint32_t n = (uint32_t)(cx1 - x + 1 < ROW ? cx1 - x + 1 : ROW);

            colours(g, 0, x, y, n, row);
            k->copy(gfx_pixel_at(rp->rp_BitMap, x, y), row, n);
        }
    }
}

bool PathRoundRect(struct GfxPath *path, const struct GfxRect *r, int32_t radius)
{
    int64_t x0, y0, x1, y1, rad, kr;
    struct GfxPoint *p;
    uint8_t *v;

    if (!path || !r || r->MaxX < r->MinX || r->MaxY < r->MinY)
        return path && r;
    x0 = (int64_t)r->MinX * 64, y0 = (int64_t)r->MinY * 64;
    x1 = ((int64_t)r->MaxX + 1) * 64, y1 = ((int64_t)r->MaxY + 1) * 64;
    rad = radius < 0 ? 0 : radius;
    rad = rad > (x1 - x0) / 2 ? (x1 - x0) / 2 : rad;
    rad = rad > (y1 - y0) / 2 ? (y1 - y0) / 2 : rad;
    kr = rad - rad * 36195 / 65536;     /* the control's distance in from the corner: 1 - 0.5523 */
    if (path->gp_NumVerbs > path->gp_MaxVerbs || path->gp_NumPoints > path->gp_MaxPoints ||
        path->gp_MaxVerbs - path->gp_NumVerbs < (rad ? 10 : 5) ||
        path->gp_MaxPoints - path->gp_NumPoints < (rad ? 17 : 4))
        return false;
    p = path->gp_Points + path->gp_NumPoints;
    v = path->gp_Verbs + path->gp_NumVerbs;
#define PT(px, py) (*p++ = (struct GfxPoint){ (int32_t)(px), (int32_t)(py) })
    *v++ = PATH_MOVE, PT(x0 + rad, y0);
    *v++ = PATH_LINE, PT(x1 - rad, y0);
    if (rad)
        *v++ = PATH_CUBIC, PT(x1 - kr, y0), PT(x1, y0 + kr), PT(x1, y0 + rad);
    *v++ = PATH_LINE, PT(x1, y1 - rad);
    if (rad)
        *v++ = PATH_CUBIC, PT(x1, y1 - kr), PT(x1 - kr, y1), PT(x1 - rad, y1);
    *v++ = PATH_LINE, PT(x0 + rad, y1);
    if (rad) {
        *v++ = PATH_CUBIC, PT(x0 + kr, y1), PT(x0, y1 - kr), PT(x0, y1 - rad);
        *v++ = PATH_LINE, PT(x0, y0 + rad);
        *v++ = PATH_CUBIC, PT(x0, y0 + kr), PT(x0 + kr, y0), PT(x0 + rad, y0);
    }
#undef PT
    *v++ = PATH_CLOSE;
    path->gp_NumPoints = (uint32_t)(p - path->gp_Points);
    path->gp_NumVerbs = (uint32_t)(v - path->gp_Verbs);
    return true;
}

void InitTmpRas(struct TmpRas *tr, void *buffer, uint64_t size)
{
    if (tr)
        *tr = (struct TmpRas){ buffer, buffer ? size : 0 };
}

void BltMask(struct RastPort *rp, const struct GfxMask *mask, int32_t x, int32_t y)
{
    gfx_blt_mask(&gfx_rows_scalar, rp, mask, x, y);
}

void FillPath(struct RastPort *rp, const struct GfxPath *path, uint32_t flags)
{
    gfx_fill_path(&gfx_rows_scalar, rp, path, flags, nullptr);
}

void FillGradient(struct RastPort *rp, const struct GfxGradient *g, const struct GfxPath *path,
                  uint32_t flags)
{
    gfx_fill_gradient(&gfx_rows_scalar, rp, g, path, flags);
}
