/*
 * lib/font/glyf.c -- a glyph's outline: loca, glyf, GlyphOutline (V8 step 1)
 *
 * A simple glyph's flags, xs and ys are three arrays one after another.
 * One pass over the flags finds where the ys start; a second walks all
 * three together, so a glyph's points go to the sink one at a time, with
 * no buffer. A composite places other glyphs, each by an offset and
 * perhaps a 2x2 matrix, at most 16 deep, and visiting 2,048 components
 * at most in all (a composite of composites can't be made to take
 * forever). A component placed by matching points is ERR_UNSUPPORTED.
 *
 * Points are scaled as they're read (16.16, rounded), then each
 * composite's matrix and offset applied, innermost first, all in the
 * scaled units: the order FreeType's unhinted loader uses, so that the
 * oracle's outlines can match to the 1/64 px. GlyphOutline's sink turns
 * the on- and off-curve points into lines and quadratics, y down, with
 * the implied on-curve point halfway between two off-curve ones.
 *
 * From ISO/IEC 14496-22 §5.3.3 and Apple's TrueType Reference Manual's
 * 'glyf' chapter.
 */
#include "sfnt.h"

constexpr unsigned MAX_DEPTH = 16;
constexpr unsigned MAX_COMPONENTS = 2048;
constexpr int64_t MAX_COORD = 1ll << 30;

/* Simple glyphs' flags */
enum : uint8_t {
    ON_CURVE = 1u << 0,
    X_SHORT = 1u << 1,
    Y_SHORT = 1u << 2,
    REPEAT = 1u << 3,
    X_SAME = 1u << 4,                   /* or, short, positive */
    Y_SAME = 1u << 5,
};

/* Composites' flags */
enum : uint16_t {
    ARGS_ARE_WORDS = 1u << 0,
    ARGS_ARE_XY = 1u << 1,
    HAVE_SCALE = 1u << 3,
    MORE_COMPONENTS = 1u << 5,
    HAVE_XY_SCALE = 1u << 6,
    HAVE_TWO_BY_TWO = 1u << 7,
    SCALED_OFFSET = 1u << 11,
    UNSCALED_OFFSET = 1u << 12,
};

/* A component's placement, scaled: x' = a x + c y + dx, y' = b x + d y + dy. */
struct xform {
    int64_t a, b, c, d;                 /* 16.16 */
    int64_t dx, dy;
    bool identity;
};

struct walk {
    const struct font *f;
    int64_t scale;
    struct font_sink *sink;
    struct xform chain[MAX_DEPTH];
    unsigned components;
};

static int64_t emit(struct walk *w, unsigned depth, int64_t x, int64_t y, bool on)
{
    x = font_mulfix(x, w->scale);
    y = font_mulfix(y, w->scale);
    for (unsigned k = depth; k-- > 0;) {
        const struct xform *t = &w->chain[k];

        if (!t->identity) {
            int64_t nx = font_mulfix(x, t->a) + font_mulfix(y, t->c);

            y = font_mulfix(x, t->b) + font_mulfix(y, t->d);
            x = nx;
        }
        x += t->dx;
        y += t->dy;
        if (x <= -MAX_COORD || x >= MAX_COORD || y <= -MAX_COORD || y >= MAX_COORD)
            return ERR_BADFONT;
    }
    return w->sink->point(w->sink, (int32_t)x, (int32_t)y, on) ? 0 : ERR_FULL;
}

/* The glyph's bytes in glyf, from loca: empty for one with no outline. */
static bool glyph_span(const struct font *f, uint32_t glyph, struct Span *out)
{
    uint64_t from, to;

    if (f->loca_long) {
        uint32_t a = 0, b = 0;

        SpanReadU32BE(f->loca, 4 * (uint64_t)glyph, &a);
        SpanReadU32BE(f->loca, 4 * (uint64_t)glyph + 4, &b);
        from = a, to = b;
    } else {
        uint16_t a = 0, b = 0;

        SpanReadU16BE(f->loca, 2 * (uint64_t)glyph, &a);
        SpanReadU16BE(f->loca, 2 * (uint64_t)glyph + 2, &b);
        from = 2 * (uint64_t)a, to = 2 * (uint64_t)b;
    }
    if (to < from)
        return false;
    return SpanSlice(f->glyf, from, to - from, out);
}

static int64_t simple(struct walk *w, unsigned depth, struct Span g, int16_t ncontours)
{
    uint64_t ends = 10, at;
    uint16_t ninstr = 0, last = 0, npoints;
    uint64_t xbytes = 0, xs, ys, fl;
    int64_t e;

    if (ncontours == 0)
        return 0;
    if (!SpanReadU16BE(g, ends + 2 * ((uint64_t)ncontours - 1), &last) ||
        !SpanReadU16BE(g, ends + 2 * (uint64_t)ncontours, &ninstr))
        return ERR_BADFONT;
    if (last == 0xffff)
        return ERR_BADFONT;
    npoints = (uint16_t)(last + 1);
    fl = ends + 2 * (uint64_t)ncontours + 2 + ninstr;

    /* Pass 1: the flags' length, and the xs'. */
    at = fl;
    for (uint32_t i = 0; i < npoints;) {
        uint8_t flag, rep = 0;

        if (!SpanReadU8(g, at++, &flag) || (flag & REPEAT && !SpanReadU8(g, at++, &rep)))
            return ERR_BADFONT;
        for (uint32_t r = 0; r <= rep && i < npoints; r++, i++)
            xbytes += flag & X_SHORT ? 1 : flag & X_SAME ? 0 : 2;
    }
    xs = at;
    ys = xs + xbytes;

    /* Pass 2: flags, xs and ys together; a contour ends at its end point. */
    int64_t x = 0, y = 0;
    uint32_t contour = 0;
    uint16_t end = 0;

    SpanReadU16BE(g, ends, &end);
    at = fl;
    for (uint32_t i = 0; i < npoints;) {
        uint8_t flag = 0, rep = 0;

        SpanReadU8(g, at++, &flag);
        if (flag & REPEAT)
            SpanReadU8(g, at++, &rep);
        for (uint32_t r = 0; r <= rep && i < npoints; r++, i++) {
            uint8_t b8;
            uint16_t b16;

            if (flag & X_SHORT) {
                if (!SpanReadU8(g, xs++, &b8))
                    return ERR_BADFONT;
                x += flag & X_SAME ? b8 : -(int64_t)b8;
            } else if (!(flag & X_SAME)) {
                if (!SpanReadU16BE(g, xs, &b16))
                    return ERR_BADFONT;
                xs += 2;
                x += (int16_t)b16;
            }
            if (flag & Y_SHORT) {
                if (!SpanReadU8(g, ys++, &b8))
                    return ERR_BADFONT;
                y += flag & Y_SAME ? b8 : -(int64_t)b8;
            } else if (!(flag & Y_SAME)) {
                if (!SpanReadU16BE(g, ys, &b16))
                    return ERR_BADFONT;
                ys += 2;
                y += (int16_t)b16;
            }
            if ((e = emit(w, depth, x, y, flag & ON_CURVE)) != 0)
                return e;
            while (i == end && contour < (uint32_t)ncontours) {     /* empty contours too */
                if (!w->sink->contour_end(w->sink))
                    return ERR_FULL;
                if (++contour < (uint32_t)ncontours) {
                    uint16_t next = 0;

                    SpanReadU16BE(g, ends + 2 * (uint64_t)contour, &next);
                    if (next < end)
                        return ERR_BADFONT;
                    end = next;
                }
            }
        }
    }
    return 0;
}

static int64_t glyph_at(struct walk *w, uint32_t glyph, unsigned depth);

static int64_t composite(struct walk *w, unsigned depth, struct Span g)
{
    uint64_t at = 10;
    uint16_t flags;

    if (depth >= MAX_DEPTH)
        return ERR_BADFONT;
    do {
        uint16_t gid, h1 = 0, h2 = 0;
        int64_t dx, dy;
        struct xform t = { 0x10000, 0, 0, 0x10000, 0, 0, true };
        int64_t e;

        if (++w->components > MAX_COMPONENTS)
            return ERR_BADFONT;
        if (!SpanReadU16BE(g, at, &flags) || !SpanReadU16BE(g, at + 2, &gid))
            return ERR_BADFONT;
        at += 4;
        if (flags & ARGS_ARE_WORDS) {
            if (!SpanReadU16BE(g, at, &h1) || !SpanReadU16BE(g, at + 2, &h2))
                return ERR_BADFONT;
            at += 4;
            dx = (int16_t)h1, dy = (int16_t)h2;
        } else {
            uint8_t b1, b2;

            if (!SpanReadU8(g, at, &b1) || !SpanReadU8(g, at + 1, &b2))
                return ERR_BADFONT;
            at += 2;
            dx = (int8_t)b1, dy = (int8_t)b2;
        }
        if (!(flags & ARGS_ARE_XY))
            return ERR_UNSUPPORTED;     /* placed by matching points */
        if (flags & (HAVE_SCALE | HAVE_XY_SCALE | HAVE_TWO_BY_TWO)) {
            uint16_t v[4] = { 0 };
            unsigned n = flags & HAVE_TWO_BY_TWO ? 4 : flags & HAVE_XY_SCALE ? 2 : 1;

            for (unsigned k = 0; k < n; k++)
                if (!SpanReadU16BE(g, at + 2 * k, &v[k]))
                    return ERR_BADFONT;
            at += 2 * n;
            /* 2.14 to 16.16 */
            if (n == 1)
                t.a = t.d = (int64_t)(int16_t)v[0] * 4;
            else if (n == 2)
                t.a = (int64_t)(int16_t)v[0] * 4, t.d = (int64_t)(int16_t)v[1] * 4;
            else
                t.a = (int64_t)(int16_t)v[0] * 4, t.b = (int64_t)(int16_t)v[1] * 4,
                t.c = (int64_t)(int16_t)v[2] * 4, t.d = (int64_t)(int16_t)v[3] * 4;
            t.identity = false;
        }
        dx = font_mulfix(dx, w->scale);
        dy = font_mulfix(dy, w->scale);
        if (!t.identity && (flags & SCALED_OFFSET) && !(flags & UNSCALED_OFFSET)) {
            int64_t nx = font_mulfix(dx, t.a) + font_mulfix(dy, t.c);

            dy = font_mulfix(dx, t.b) + font_mulfix(dy, t.d);
            dx = nx;
        }
        t.dx = dx;
        t.dy = dy;
        w->chain[depth] = t;
        if (gid >= w->f->nglyphs)
            return ERR_BADFONT;
        if ((e = glyph_at(w, gid, depth + 1)) != 0)
            return e;
    } while (flags & MORE_COMPONENTS);
    return 0;
}

static int64_t glyph_at(struct walk *w, uint32_t glyph, unsigned depth)
{
    struct Span g;
    uint16_t n;

    if (!glyph_span(w->f, glyph, &g))
        return ERR_BADFONT;
    if (g.sp_Len == 0)
        return 0;
    if (!SpanReadU16BE(g, 0, &n) || g.sp_Len < 10)
        return ERR_BADFONT;
    return (int16_t)n >= 0 ? simple(w, depth, g, (int16_t)n) : composite(w, depth, g);
}

int64_t font_glyph_points(const struct font *f, uint32_t glyph, int64_t scale, struct font_sink *sink)
{
    struct walk w = { .f = f, .scale = scale, .sink = sink };

    if (glyph >= f->nglyphs)
        return ERR_BADARG;
    return glyph_at(&w, glyph, 0);
}

/* --- GlyphOutline's sink: points to a path --- */

struct to_path {
    struct font_sink s;
    struct GfxPath *p;
    uint32_t n;                         /* the contour's points so far */
    uint32_t move;                      /* its MOVE's point, filled at its end if it began off */
    struct GfxPoint first, last, cur, ctl;
    bool first_on, last_on, pending;
};

static bool add(struct to_path *t, uint8_t verb, struct GfxPoint a, struct GfxPoint b, unsigned npts)
{
    struct GfxPath *p = t->p;

    if (p->gp_NumVerbs >= p->gp_MaxVerbs || p->gp_MaxPoints - p->gp_NumPoints < npts)
        return false;
    p->gp_Verbs[p->gp_NumVerbs++] = verb;
    if (npts > 0)
        p->gp_Points[p->gp_NumPoints++] = a;
    if (npts > 1)
        p->gp_Points[p->gp_NumPoints++] = b;
    t->cur = npts == 2 ? b : a;
    return true;
}

static struct GfxPoint mid(struct GfxPoint a, struct GfxPoint b)
{
    return (struct GfxPoint){ (a.x + b.x) / 2, (a.y + b.y) / 2 };
}

static bool path_point(struct font_sink *s, int32_t x, int32_t y, bool on)
{
    struct to_path *t = (struct to_path *)s;
    struct GfxPoint p = { x, -y };

    t->last = p;
    t->last_on = on;
    if (t->n++ == 0) {
        t->move = t->p->gp_NumPoints;
        t->first = p;
        t->first_on = on;
        t->pending = !on;
        t->ctl = p;
        return add(t, PATH_MOVE, p, p, 1);     /* its point fixed at the end if p is off */
    }
    if (on) {
        bool ok = t->pending ? add(t, PATH_QUAD, t->ctl, p, 2) : add(t, PATH_LINE, p, p, 1);

        t->pending = false;
        return ok;
    }
    if (t->pending && !add(t, PATH_QUAD, t->ctl, mid(t->ctl, p), 2))
        return false;
    t->pending = true;
    t->ctl = p;
    return true;
}

static bool path_end(struct font_sink *s)
{
    struct to_path *t = (struct to_path *)s;
    struct GfxPoint start = t->first;

    if (t->n == 0)
        return true;
    t->n = 0;
    if (!t->first_on) {                 /* it began off-curve: start at the last point, or between */
        start = t->last_on ? t->last : mid(t->first, t->last);
        t->p->gp_Points[t->move] = start;
    }
    if (t->pending) {
        if (!add(t, PATH_QUAD, t->ctl, start, 2))
            return false;
    } else if (t->cur.x != start.x || t->cur.y != start.y) {
        if (!add(t, PATH_LINE, start, start, 1))
            return false;
    }
    t->pending = false;
    return add(t, PATH_CLOSE, start, start, 0);
}

int64_t GlyphOutline(const struct FontFace *face, uint32_t glyph, uint32_t size, struct GfxPath *path)
{
    const struct font *f = font_of(face);
    struct to_path t = { .s = { path_point, path_end }, .p = path };
    int64_t e;

    path->gp_NumPoints = path->gp_NumVerbs = 0;
    if (size == 0 || size > (4096u << 6))
        return ERR_BADARG;
    e = font_glyph_points(f, glyph, font_divfix(size, f->upem), &t.s);
    if (e == 0 && t.n)                  /* a glyph that ended mid-contour */
        e = ERR_BADFONT;
    if (e != 0)
        path->gp_NumPoints = path->gp_NumVerbs = 0;
    return e;
}
