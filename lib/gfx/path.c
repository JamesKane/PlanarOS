/*
 * lib/gfx/path.c -- a path's coverage: PathBounds, PathMask (V8 step 1)
 *
 * The rasteriser font.library's glyphs and gfx's own paths share
 * (v8-plan decision 6). Curves are cut into lines no further than 1/512
 * px from them, at evenly spaced points, as many as the curve's bend
 * needs: fine enough that every pixel is within 2/255 of the exact
 * coverage (tools/font-oracle, pass 4), where 1/8 px was a tenth out.
 * Each line's exact signed area is then added up pixel by pixel: in a
 * pixel it crosses, the part of the pixel to the line's right, and in
 * every pixel right of that, the whole height it spans. A sweep along
 * each row sums those into each pixel's coverage, clamped (non-zero
 * winding) or folded (even-odd): the sweep is a row kernel (rows.h), the
 * vector unit's where there is one (V8 step 3). All of it is integer
 * arithmetic, in 1/256 px, so every CPU and the host get the same bytes.
 *
 * Units inside: a coordinate is 24.8 (the path's 26.6 times 4), and an
 * area is in 1/(256 * 512) of a pixel, so a pixel's whole coverage is
 * GFX_COVER_FULL (rows.h): a line crossing a row's full height (256) adds 256 * 512
 * to the pixels right of it.
 */
#include <stddef.h>

#include "rows.h"

constexpr int64_t SUB = 256;                    /* subpixels a pixel */
constexpr int64_t FLAT_DIV = 512;              /* a curve's lines: within 1/FLAT_DIV px of it */
constexpr int64_t MAX_STEPS = 4096;             /* lines a curve is cut into, at most */

/* a / b rounded down, b > 0. */
static int64_t fdiv(int64_t a, int64_t b)
{
    int64_t q = a / b;

    return (a % b != 0 && (a < 0)) ? q - 1 : q;
}

static int64_t iabs(int64_t v)
{
    return v < 0 ? -v : v;
}

/* The smallest n with n * n >= v. */
static int64_t isqrt_up(int64_t v)
{
    int64_t n = 0, bit = 1ll << 31;

    if (v <= 0)
        return 0;
    for (; bit; bit >>= 1)              /* the largest n with n * n <= v */
        if ((n + bit) * (n + bit) <= v)
            n += bit;
    return n * n == v ? n : n + 1;
}

struct raster {
    int32_t *acc;                       /* (width + 2) a row, each pixel's added area */
    int64_t width, height, stride;
    int64_t sx, sy;                     /* the subpath's start, 24.8, the mask's own */
    int64_t cx, cy;                     /* where it's got to */
    bool open;
};

/* A vertical piece at x, its ys 0 to SUB within the row. */
static void row_vertical(int32_t *row, int64_t x, int64_t ya, int64_t yb, int64_t dir)
{
    int64_t c = x / SUB, fx = x - c * SUB, dy = (yb - ya) * dir;
    int64_t own = dy * (2 * SUB - 2 * fx);

    row[c] += (int32_t)own;
    row[c + 1] += (int32_t)(dy * 2 * SUB - own);
}

/*
 * A line's piece within one row, its ys 0 to SUB within it: its area
 * into that row's pixels. What's left of the mask is a vertical piece
 * at its left edge, which still covers the pixels right of it, and
 * what's right of it covers none. Where it crosses a pixel's edge, y is
 * found from the piece's own ends, so a mask that's part of a bigger
 * one has the same bytes there as the bigger one (FillPath's pieces).
 */
static void row_piece(int32_t *row, int64_t width, int64_t xa, int64_t ya, int64_t xb, int64_t yb,
                      int64_t dir)
{
    int64_t right = width * SUB;
#define Y_AT(x) (ya + fdiv((yb - ya) * iabs((x) - xa), iabs(xb - xa)))

    if (xa == xb || (xa <= 0 && xb <= 0) || (xa >= right && xb >= right)) {
        if (xa < right || xb < right)
            row_vertical(row, xa < 0 ? 0 : xa > right ? right : xa, ya, yb, dir);
        return;
    }
    bool rightward = xb > xa;
    int64_t px = xa, py = ya, ex = xb, ey = yb;

    if (rightward ? xa < 0 : xb < 0) {          /* the part left of the mask */
        int64_t y0 = Y_AT(0);

        if (rightward) {
            row_vertical(row, 0, ya, y0, dir);
            px = 0, py = y0;
        } else {
            row_vertical(row, 0, y0, yb, dir);
            ex = 0, ey = y0;
        }
    }
    if (rightward ? xb > right : xa > right) {  /* right of it: nothing */
        if (rightward)
            ex = right, ey = Y_AT(right);
        else
            px = right, py = Y_AT(right);
    }
    /* Cell by cell from (px, py) to (ex, ey), cut where it crosses a pixel's edge. */
    int64_t c = rightward ? px / SUB : (px - 1) / SUB;
    int64_t last = rightward ? (ex - 1) / SUB : ex / SUB;

    if (rightward ? ex <= px : ex >= px)
        return;
    for (;;) {
        int64_t qx, qy;

        if (c == last) {
            qx = ex;
            qy = ey;
        } else {
            qx = rightward ? (c + 1) * SUB : c * SUB;
            qy = Y_AT(qx);
        }
        int64_t dy = (qy - py) * dir;
        int64_t own = dy * (2 * SUB - (px - c * SUB) - (qx - c * SUB));

        row[c] += (int32_t)own;
        row[c + 1] += (int32_t)(dy * 2 * SUB - own);
        if (c == last)
            return;
        px = qx;
        py = qy;
        c += rightward ? 1 : -1;
    }
#undef Y_AT
}

/* A line, 24.8 in the mask's own coordinates: its area, row by row. */
static void line(struct raster *r, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    int64_t dir = 1, top, bottom;

    if (y0 == y1)
        return;
    if (y0 > y1) {
        int64_t t;

        t = x0, x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
        dir = -1;
    }
    top = y0 < 0 ? 0 : y0;
    bottom = y1 > r->height * SUB ? r->height * SUB : y1;
    for (int64_t row = top / SUB; row * SUB < bottom; row++) {
        int64_t ya = row * SUB > top ? row * SUB : top;
        int64_t yb = (row + 1) * SUB < bottom ? (row + 1) * SUB : bottom;
        int64_t xa = ya == y0 ? x0 : x0 + fdiv((x1 - x0) * (ya - y0), y1 - y0);
        int64_t xb = yb == y1 ? x1 : x0 + fdiv((x1 - x0) * (yb - y0), y1 - y0);

        if (ya < yb)
            row_piece(r->acc + row * r->stride, r->width, xa, ya - row * SUB, xb, yb - row * SUB, dir);
    }
}

static void line_to(struct raster *r, int64_t x, int64_t y)
{
    line(r, r->cx, r->cy, x, y);
    r->cx = x;
    r->cy = y;
    r->open = true;                     /* after a CLOSE, drawing reopens it */
}

static void close_sub(struct raster *r)
{
    if (r->open)
        line_to(r, r->sx, r->sy);
    r->open = false;
}

/* A quadratic: 'n' lines, n chosen so each is within 1/FLAT_DIV px of it. Its
 * worst distance from n even chords is |p0 - 2 p1 + p2| / (4 n^2). */
static void quad_to(struct raster *r, int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    int64_t x0 = r->cx, y0 = r->cy;
    int64_t bend = iabs(x0 - 2 * x1 + x2) + iabs(y0 - 2 * y1 + y2);
    int64_t n = isqrt_up(fdiv(bend * FLAT_DIV + 4 * SUB - 1, 4 * SUB));

    n = n < 1 ? 1 : n > MAX_STEPS ? MAX_STEPS : n;
    for (int64_t i = 1; i < n; i++) {
        int64_t a = (n - i) * (n - i), b = 2 * i * (n - i), c = i * i, nn = n * n;

        line_to(r, fdiv(a * x0 + b * x1 + c * x2 + nn / 2, nn), fdiv(a * y0 + b * y1 + c * y2 + nn / 2, nn));
    }
    line_to(r, x2, y2);
}

/* A cubic likewise: within 3 max|second difference| / (4 n^2). */
static void cubic_to(struct raster *r, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t x3,
                     int64_t y3)
{
    int64_t x0 = r->cx, y0 = r->cy;
    int64_t b1 = iabs(x0 - 2 * x1 + x2) + iabs(y0 - 2 * y1 + y2);
    int64_t b2 = iabs(x1 - 2 * x2 + x3) + iabs(y1 - 2 * y2 + y3);
    int64_t bend = 3 * (b1 > b2 ? b1 : b2);
    int64_t n = isqrt_up(fdiv(bend * FLAT_DIV + 4 * SUB - 1, 4 * SUB));

    n = n < 1 ? 1 : n > MAX_STEPS ? MAX_STEPS : n;
    for (int64_t i = 1; i < n; i++) {
        int64_t u = n - i, nnn = n * n * n;
        int64_t a = u * u * u, b = 3 * u * u * i, c = 3 * u * i * i, d = i * i * i;

        line_to(r, fdiv(a * x0 + b * x1 + c * x2 + d * x3 + nnn / 2, nnn),
                fdiv(a * y0 + b * y1 + c * y2 + d * y3 + nnn / 2, nnn));
    }
    line_to(r, x3, y3);
}

/* Points a verb takes; -1 for one that isn't. */
static int verb_points(uint8_t v)
{
    switch (v) {
    case PATH_MOVE:
    case PATH_LINE: return 1;
    case PATH_QUAD: return 2;
    case PATH_CUBIC: return 3;
    case PATH_CLOSE: return 0;
    }
    return -1;
}

/* The path's verbs and points agree, fit in their arrays, and it
 * starts with a MOVE. */
static bool path_ok(const struct GfxPath *p)
{
    uint64_t need = 0;

    if (p->gp_NumVerbs > p->gp_MaxVerbs || p->gp_NumPoints > p->gp_MaxPoints)
        return false;
    for (uint32_t i = 0; i < p->gp_NumVerbs; i++) {
        int n = verb_points(p->gp_Verbs[i]);

        if (n < 0 || (i == 0 && p->gp_Verbs[i] != PATH_MOVE))
            return false;
        need += (uint64_t)n;
    }
    return need == p->gp_NumPoints;
}

bool PathBounds(const struct GfxPath *path, struct GfxRect *r)
{
    int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;

    if (!path_ok(path) || !path->gp_NumPoints)
        return false;
    for (uint32_t i = 0; i < path->gp_NumPoints; i++) {
        const struct GfxPoint *p = &path->gp_Points[i];

        x0 = p->x < x0 ? p->x : x0;
        y0 = p->y < y0 ? p->y : y0;
        x1 = p->x > x1 ? p->x : x1;
        y1 = p->y > y1 ? p->y : y1;
    }
    x0 = fdiv(x0, 64);
    y0 = fdiv(y0, 64);
    x1 = -fdiv(-x1, 64);                /* rounded up: the pixel past the last touched */
    y1 = -fdiv(-y1, 64);
    if (x1 <= x0 || y1 <= y0)
        return false;
    *r = (struct GfxRect){ (int32_t)x0, (int32_t)y0, (int32_t)(x1 - 1), (int32_t)(y1 - 1) };
    return true;
}

uint64_t PathMaskScratch(uint32_t width, uint32_t height)
{
    return ((uint64_t)width + 2) * height * sizeof(int32_t);
}

bool gfx_path_mask(const struct gfx_rows *k, const struct GfxPath *path, uint32_t flags,
                   struct GfxMask *mask, void *scratch, uint64_t scratchSize)
{
    struct raster r = {
        .acc = scratch, .width = mask->gm_Width, .height = mask->gm_Height, .stride = (int64_t)mask->gm_Width + 2,
    };
    int64_t ox = (int64_t)mask->gm_X * SUB, oy = (int64_t)mask->gm_Y * SUB;
    const struct GfxPoint *pt = path->gp_Points;

    if (!path_ok(path) || mask->gm_Width > GFX_MAX_SIZE || mask->gm_Height > GFX_MAX_SIZE ||
        mask->gm_BytesPerRow < mask->gm_Width ||
        scratchSize < PathMaskScratch(mask->gm_Width, mask->gm_Height))
        return false;
    memset(scratch, 0, PathMaskScratch(mask->gm_Width, mask->gm_Height));
    /* 26.6 to 24.8, less the mask's place. */
#define X(i) ((int64_t)pt[i].x * 4 - ox)
#define Y(i) ((int64_t)pt[i].y * 4 - oy)
    for (uint32_t v = 0; v < path->gp_NumVerbs; v++) {
        switch (path->gp_Verbs[v]) {
        case PATH_MOVE:
            close_sub(&r);
            r.sx = r.cx = X(0);
            r.sy = r.cy = Y(0);
            r.open = true;
            break;
        case PATH_LINE:
            line_to(&r, X(0), Y(0));
            break;
        case PATH_QUAD:
            quad_to(&r, X(0), Y(0), X(1), Y(1));
            break;
        case PATH_CUBIC:
            cubic_to(&r, X(0), Y(0), X(1), Y(1), X(2), Y(2));
            break;
        case PATH_CLOSE:
            close_sub(&r);
            break;
        }
        pt += verb_points(path->gp_Verbs[v]);
    }
#undef X
#undef Y
    close_sub(&r);
    for (int64_t y = 0; y < r.height; y++)
        k->sweep(mask->gm_Pixels + y * mask->gm_BytesPerRow, r.acc + y * r.stride, (uint64_t)r.width,
                 flags & PATHF_EVENODD);
    return true;
}

bool PathMask(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
              uint64_t scratchSize)
{
    return gfx_path_mask(&gfx_rows_scalar, path, flags, mask, scratch, scratchSize);
}
