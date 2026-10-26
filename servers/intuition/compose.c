/*
 * servers/intuition/compose.c -- composing the screen (v7-plan steps 3
 * and 4)
 *
 * A rectangle to repaint is taken by the windows front to back: the
 * front window it meets gives the part they share, blits from the
 * bitmaps of the window's parts, and the up to four pieces of it beside
 * that go on to the windows behind. What no window covers is the
 * backdrop. So each pixel is written once, and the screen is never read.
 */
#include "compose.h"

static bool meets(struct GfxRect a, struct GfxRect b)
{
    return a.MinX <= b.MaxX && b.MinX <= a.MaxX && a.MinY <= b.MaxY && b.MinY <= a.MaxY;
}

static struct GfxRect bounds(struct GfxRect a, struct GfxRect b)
{
    return (struct GfxRect){
        a.MinX < b.MinX ? a.MinX : b.MinX, a.MinY < b.MinY ? a.MinY : b.MinY,
        a.MaxX > b.MaxX ? a.MaxX : b.MaxX, a.MaxY > b.MaxY ? a.MaxY : b.MaxY,
    };
}

static struct GfxRect common(struct GfxRect a, struct GfxRect b)
{
    return (struct GfxRect){
        a.MinX > b.MinX ? a.MinX : b.MinX, a.MinY > b.MinY ? a.MinY : b.MinY,
        a.MaxX < b.MaxX ? a.MaxX : b.MaxX, a.MaxY < b.MaxY ? a.MaxY : b.MaxY,
    };
}

/* The rectangle (x0, y0) to (x1, y1), clipped to the screen; false if
 * none of it is on it. */
static bool on_screen(const struct ic_screen *s, int64_t x0, int64_t y0, int64_t x1, int64_t y1,
                      struct GfxRect *r)
{
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int64_t)s->width - 1)
        x1 = (int64_t)s->width - 1;
    if (y1 > (int64_t)s->height - 1)
        y1 = (int64_t)s->height - 1;
    if (x0 > x1 || y0 > y1)
        return false;
    *r = (struct GfxRect){ (int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1 };
    return true;
}

static bool window_rect(const struct ic_screen *s, const struct ic_window *w, struct GfxRect *r)
{
    return w->w && w->h &&
           on_screen(s, w->x, w->y, (int64_t)w->x + w->w - 1, (int64_t)w->y + w->h - 1, r);
}

static void add(struct ic_damage *d, struct GfxRect r)
{
    for (unsigned i = 0; i < d->n;)
        if (meets(d->r[i], r)) {
            r = bounds(r, d->r[i]);
            d->r[i] = d->r[--d->n];
            i = 0;
        } else {
            i++;
        }
    if (d->n == IC_DAMAGE) {
        for (unsigned i = 0; i < d->n; i++)
            r = bounds(r, d->r[i]);
        d->n = 0;
    }
    d->r[d->n++] = r;
}

void ic_init(struct ic_screen *s, const struct GfxIFace *gfx, uint32_t width, uint32_t height)
{
    *s = (struct ic_screen){ .gfx = gfx, .width = width, .height = height, .backdrop = IC_BACKDROP };
    ic_damage(s, (struct GfxRect){ 0, 0, (int32_t)width - 1, (int32_t)height - 1 });
}

void ic_damage(struct ic_screen *s, struct GfxRect r)
{
    if (r.MinX <= r.MaxX && r.MinY <= r.MaxY && on_screen(s, r.MinX, r.MinY, r.MaxX, r.MaxY, &r))
        add(&s->now, r);
}

void ic_damage_window(struct ic_screen *s, const struct ic_window *w, struct GfxRect r)
{
    struct GfxRect in;

    if (r.MinX > r.MaxX || r.MinY > r.MaxY || !w->w || !w->h)
        return;
    r = common(r, (struct GfxRect){ 0, 0, (int32_t)w->w - 1, (int32_t)w->h - 1 });
    if (r.MinX <= r.MaxX && r.MinY <= r.MaxY &&
        on_screen(s, (int64_t)w->x + r.MinX, (int64_t)w->y + r.MinY, (int64_t)w->x + r.MaxX,
                  (int64_t)w->y + r.MaxY, &in))
        add(&s->now, in);
}

static void damage_whole(struct ic_screen *s, const struct ic_window *w)
{
    struct GfxRect r;

    if (window_rect(s, w, &r))
        add(&s->now, r);
}

bool ic_add(struct ic_screen *s, struct ic_window *w)
{
    if (s->nz == IC_WINDOWS)
        return false;
    for (unsigned i = s->nz; i > 0; i--)
        s->z[i] = s->z[i - 1];
    s->z[0] = w;
    s->nz++;
    damage_whole(s, w);
    return true;
}

void ic_remove(struct ic_screen *s, struct ic_window *w)
{
    for (unsigned i = 0; i < s->nz; i++)
        if (s->z[i] == w) {
            damage_whole(s, w);
            for (s->nz--; i < s->nz; i++)
                s->z[i] = s->z[i + 1];
            return;
        }
}

void ic_raise(struct ic_screen *s, struct ic_window *w)
{
    for (unsigned i = 0; i < s->nz; i++)
        if (s->z[i] == w) {
            for (; i > 0; i--)
                s->z[i] = s->z[i - 1];
            s->z[0] = w;
            damage_whole(s, w);
            return;
        }
}

void ic_lower(struct ic_screen *s, struct ic_window *w)
{
    for (unsigned i = 0; i < s->nz; i++)
        if (s->z[i] == w) {
            for (; i + 1 < s->nz; i++)
                s->z[i] = s->z[i + 1];
            s->z[s->nz - 1] = w;
            damage_whole(s, w);
            return;
        }
}

struct ic_window *ic_at(const struct ic_screen *s, int32_t x, int32_t y)
{
    for (unsigned i = 0; i < s->nz; i++) {
        struct ic_window *w = s->z[i];

        if (w->nparts && x >= w->x && y >= w->y && x < (int64_t)w->x + w->w &&
            y < (int64_t)w->y + w->h)
            return w;
    }
    return nullptr;
}

void ic_move(struct ic_screen *s, struct ic_window *w, int32_t x, int32_t y)
{
    damage_whole(s, w);
    w->x = x;
    w->y = y;
    damage_whole(s, w);
}

bool ic_dirty(const struct ic_screen *s)
{
    return s->now.n != 0;
}

static uint64_t pixels(struct GfxRect r)
{
    return (uint64_t)(r.MaxX - r.MinX + 1) * (uint64_t)(r.MaxY - r.MinY + 1);
}

/* What ic_compose paints: each pixel of 'all' once. */
uint64_t ic_pending(const struct ic_screen *s)
{
    struct ic_damage all = s->last;
    uint64_t n = 0;

    for (unsigned i = 0; i < s->now.n; i++)
        add(&all, s->now.r[i]);
    for (unsigned i = 0; i < all.n; i++)
        n += pixels(all.r[i]);
    return n;
}

uint64_t ic_start(uint64_t now, uint64_t vblank, uint64_t period, uint64_t lead, uint64_t cost,
                  bool waited)
{
    uint64_t due;

    if (waited || cost >= period / 2 || lead >= period / 2)
        return 0;
    due = vblank + period - lead - cost;
    return now < due ? due : 0;
}

/* 'r', from the windows at depth z and behind. */
static void paint(struct ic_screen *s, struct RastPort *rp, struct GfxRect r, unsigned z)
{
    for (; z < s->nz; z++) {
        const struct ic_window *w = s->z[z];
        struct GfxRect wr, i;

        if (!w->nparts || !window_rect(s, w, &wr) || !meets(r, wr))
            continue;
        i = common(r, wr);
        for (unsigned k = 0; k < w->nparts; k++) {
            const struct ic_part *p = &w->part[k];
            struct GfxRect pr = { w->x + p->r.MinX, w->y + p->r.MinY, w->x + p->r.MaxX,
                                  w->y + p->r.MaxY }, c;

            if (!meets(i, pr))
                continue;
            c = common(i, pr);
            s->gfx->BltBitMap(p->bm, p->sx + c.MinX - pr.MinX, p->sy + c.MinY - pr.MinY, rp, c.MinX,
                              c.MinY, c.MaxX - c.MinX + 1, c.MaxY - c.MinY + 1, BLEND_SRC);
        }
        s->painted += pixels(i);
        if (r.MinY < i.MinY)
            paint(s, rp, (struct GfxRect){ r.MinX, r.MinY, r.MaxX, i.MinY - 1 }, z + 1);
        if (r.MaxY > i.MaxY)
            paint(s, rp, (struct GfxRect){ r.MinX, i.MaxY + 1, r.MaxX, r.MaxY }, z + 1);
        if (r.MinX < i.MinX)
            paint(s, rp, (struct GfxRect){ r.MinX, i.MinY, i.MinX - 1, i.MaxY }, z + 1);
        if (r.MaxX > i.MaxX)
            paint(s, rp, (struct GfxRect){ i.MaxX + 1, i.MinY, r.MaxX, i.MaxY }, z + 1);
        return;
    }
    rp->rp_FgPen = s->backdrop;
    s->gfx->RectFill(rp, r.MinX, r.MinY, r.MaxX, r.MaxY);
    s->painted += pixels(r);
}

void ic_compose(struct ic_screen *s, struct BitMap *target)
{
    struct ic_damage all = s->last;
    struct RastPort rp;

    s->gfx->InitRastPort(&rp, target);
    for (unsigned i = 0; i < s->now.n; i++)
        add(&all, s->now.r[i]);
    for (unsigned i = 0; i < all.n; i++)
        paint(s, &rp, all.r[i], 0);
    s->last = s->now;
    s->now.n = 0;
}
