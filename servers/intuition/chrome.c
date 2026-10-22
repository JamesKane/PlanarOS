/*
 * servers/intuition/chrome.c -- a window's chrome (v7-plan step 4)
 *
 * Drawn as the window's coordinates have it, each rectangle cut into
 * the pieces of it that 'top' and 'side' hold. §13.2's colours: the
 * title bar's win.frame gradient, the frame's lower stop, win.bevel's
 * light along the top and left and dark around the outside, and the
 * inside sunk into it. The title is in the 8x8 font.
 */
#include "chrome.h"

constexpr uint32_t TB = INTUI_TITLEBAR, BD = INTUI_BORDER;
constexpr int32_t GADGET_W = 22, GADGET_H = 20, GADGET_Y = 3;

struct pens {
    uint32_t top, bottom;               /* win.frame's gradient */
    uint32_t light, dark;               /* win.bevel's */
    uint32_t text;
};

static const struct pens active = { 0xff5b88c2, 0xff3a6298, 0xff8bb4e8, 0xff162030, 0xfff0f4fa };
static const struct pens inactive = { 0xff4a5668, 0xff333c4b, 0xff6a788e, 0xff162030, 0xff7e8ca3 };

struct canvas {
    const struct GfxIFace *g;
    const struct chrome *c;
    struct RastPort top, side;
};

static int32_t min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

/* (x0, y0) to (x1, y1) of the window, within [rx0, rx1] by [ry0, ry1],
 * filled at (dx, dy) of the region's own top left in rp. */
static void piece(struct canvas *k, struct RastPort *rp, int32_t x0, int32_t y0, int32_t x1,
                  int32_t y1, int32_t rx0, int32_t ry0, int32_t rx1, int32_t ry1, int32_t dx,
                  int32_t dy)
{
    x0 = max32(x0, rx0), y0 = max32(y0, ry0), x1 = min32(x1, rx1), y1 = min32(y1, ry1);
    if (x0 <= x1 && y0 <= y1)
        k->g->RectFill(rp, x0 - rx0 + dx, y0 - ry0 + dy, x1 - rx0 + dx, y1 - ry0 + dy);
}

static void fill(struct canvas *k, uint32_t pen, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int32_t w = (int32_t)k->c->w, h = (int32_t)k->c->h;

    k->top.rp_FgPen = k->side.rp_FgPen = pen;
    piece(k, &k->top, x0, y0, x1, y1, 0, 0, w - 1, (int32_t)TB - 1, 0, 0);
    piece(k, &k->top, x0, y0, x1, y1, 0, h - (int32_t)BD, w - 1, h - 1, 0, (int32_t)TB);
    piece(k, &k->side, x0, y0, x1, y1, 0, (int32_t)TB, (int32_t)BD - 1, h - (int32_t)BD - 1, 0, 0);
    piece(k, &k->side, x0, y0, x1, y1, w - (int32_t)BD, (int32_t)TB, w - 1, h - (int32_t)BD - 1,
          (int32_t)BD, 0);
}

/* A bevel's two edges: light top and left, dark bottom and right. */
static void bevel(struct canvas *k, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t light,
                  uint32_t dark)
{
    fill(k, light, x0, y0, x1, y0);
    fill(k, light, x0, y0, x0, y1);
    fill(k, dark, x0 + 1, y1, x1, y1);
    fill(k, dark, x1, y0 + 1, x1, y1);
}

/* The gadget slots from the right: the close gadget's, then the depth one's. */
static bool gadget(const struct chrome *c, unsigned which, struct GfxRect *r)
{
    int32_t slot = 0, x1;

    if (c->flags & WFLG_BORDERLESS || !(c->flags & (which == CH_CLOSE ? WFLG_CLOSEGADGET
                                                                       : WFLG_DEPTHGADGET)))
        return false;
    if (which == CH_DEPTH && c->flags & WFLG_CLOSEGADGET)
        slot = 1;
    x1 = (int32_t)c->w - (int32_t)BD - 1 - slot * (GADGET_W + 2);
    *r = (struct GfxRect){ x1 - GADGET_W + 1, GADGET_Y, x1, GADGET_Y + GADGET_H - 1 };
    return r->MinX > (int32_t)BD;
}

static void draw_gadget(struct canvas *k, const struct pens *p, unsigned which)
{
    struct GfxRect r;
    bool down = k->c->pressed == which;
    int32_t cx, cy;

    if (!gadget(k->c, which, &r))
        return;
    fill(k, down ? p->bottom : p->top, r.MinX, r.MinY, r.MaxX, r.MaxY);
    bevel(k, r.MinX, r.MinY, r.MaxX, r.MaxY, down ? p->dark : p->light, down ? p->light : p->dark);
    cx = (r.MinX + r.MaxX) / 2;
    cy = (r.MinY + r.MaxY) / 2;
    if (which == CH_CLOSE) {
        for (int32_t i = -4; i <= 4; i++) {         /* a cross, 2 pixels thick */
            fill(k, p->text, cx + i, cy + i, cx + i + 1, cy + i);
            fill(k, p->text, cx - i, cy + i, cx - i + 1, cy + i);
        }
    } else {                                        /* two windows, one over the other */
        fill(k, p->dark, cx - 6, cy - 5, cx + 2, cy + 1);
        fill(k, p->bottom, cx - 5, cy - 4, cx + 1, cy);
        fill(k, p->dark, cx - 2, cy - 1, cx + 6, cy + 5);
        fill(k, p->text, cx - 1, cy, cx + 5, cy + 4);
    }
}

void chrome_size(uint32_t flags, uint32_t iw, uint32_t ih, struct chrome *c, int32_t *ix,
                 int32_t *iy)
{
    bool bare = flags & WFLG_BORDERLESS;

    c->flags = flags;
    c->w = iw + (bare ? 0 : 2 * BD);
    c->h = ih + (bare ? 0 : TB + BD);
    *ix = bare ? 0 : (int32_t)BD;
    *iy = bare ? 0 : (int32_t)TB;
}

void chrome_draw(const struct GfxIFace *g, struct BitMap *top, struct BitMap *side,
                 const struct chrome *c)
{
    const struct pens *p = c->active ? &active : &inactive;
    struct canvas k = { .g = g, .c = c };
    int32_t w = (int32_t)c->w, h = (int32_t)c->h, tx1 = w - (int32_t)BD - 5;
    struct GfxRect r;
    uint64_t n = 0;

    if (c->flags & WFLG_BORDERLESS)
        return;
    g->InitRastPort(&k.top, top);
    g->InitRastPort(&k.side, side);
    fill(&k, p->bottom, 0, 0, w - 1, h - 1);
    for (int32_t y = 1; y < (int32_t)TB - 1; y++) {
        uint32_t pen = 0xff000000;

        for (unsigned s = 0; s < 24; s += 8) {      /* each channel, top to bottom */
            int32_t a = (int32_t)(p->top >> s & 0xff), b = (int32_t)(p->bottom >> s & 0xff);

            pen |= (uint32_t)(a + (b - a) * (y - 1) / ((int32_t)TB - 3)) << s;
        }
        fill(&k, pen, 1, y, w - 2, y);
    }
    fill(&k, p->dark, 0, 0, w - 1, 0);              /* the outside */
    fill(&k, p->dark, 0, h - 1, w - 1, h - 1);
    fill(&k, p->dark, 0, 0, 0, h - 1);
    fill(&k, p->dark, w - 1, 0, w - 1, h - 1);
    fill(&k, p->light, 1, 1, w - 2, 1);             /* the light from the top left */
    fill(&k, p->light, 1, 1, 1, h - 2);
    bevel(&k, (int32_t)BD - 1, (int32_t)TB - 1, w - (int32_t)BD, h - (int32_t)BD, p->dark, p->light);
    draw_gadget(&k, p, CH_DEPTH);
    draw_gadget(&k, p, CH_CLOSE);

    if (gadget(c, CH_DEPTH, &r) || gadget(c, CH_CLOSE, &r))   /* the leftmost */
        tx1 = r.MinX - 4;
    while (c->title && n < INTUI_TITLE && c->title[n])
        n++;
    k.top.rp_Clip = (struct GfxRect){ 6, 2, tx1, (int32_t)TB - 3 };
    k.top.rp_FgPen = p->text;
    k.top.rp_DrawMode = JAM1;
    g->Text(&k.top, 10, ((int32_t)TB - 8) / 2, c->title ? c->title : "", n);
}

void chrome_parts(const struct chrome *c, const struct BitMap *top, const struct BitMap *side,
                  struct ic_window *w)
{
    int32_t cw = (int32_t)c->w, ch = (int32_t)c->h, tb = (int32_t)TB, bd = (int32_t)BD;

    if (c->flags & WFLG_BORDERLESS) {
        w->nparts = 1;
        return;
    }
    w->part[1] = (struct ic_part){ { 0, 0, cw - 1, tb - 1 }, top, 0, 0 };
    w->part[2] = (struct ic_part){ { 0, ch - bd, cw - 1, ch - 1 }, top, 0, tb };
    w->part[3] = (struct ic_part){ { 0, tb, bd - 1, ch - bd - 1 }, side, 0, 0 };
    w->part[4] = (struct ic_part){ { cw - bd, tb, cw - 1, ch - bd - 1 }, side, bd, 0 };
    w->nparts = 5;
}

unsigned chrome_hit(const struct chrome *c, int32_t x, int32_t y)
{
    int32_t w = (int32_t)c->w, h = (int32_t)c->h;
    struct GfxRect r;

    if (x < 0 || y < 0 || x >= w || y >= h)
        return CH_NONE;
    if (c->flags & WFLG_BORDERLESS)
        return CH_INSIDE;
    if (x >= (int32_t)BD && x < w - (int32_t)BD && y >= (int32_t)TB && y < h - (int32_t)BD)
        return CH_INSIDE;
    for (unsigned which = CH_DEPTH; which <= CH_CLOSE; which++)
        if (gadget(c, which, &r) && x >= r.MinX && x <= r.MaxX && y >= r.MinY && y <= r.MaxY)
            return which;
    return y < (int32_t)TB ? CH_TITLE : CH_FRAME;
}
