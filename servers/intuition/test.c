/*
 * servers/intuition/test.c -- composition, the chrome and the events on
 * the host (make intuition-test)
 *
 * compose.c with the sanitizers, over made-up windows and a fake display:
 * two screen buffers, composed in turn as main.c does. After every frame
 * the buffer just composed is checked pixel for pixel against a naive
 * compositor that redraws the whole screen from the windows, front to
 * back. The windows are opened, moved, presented, raised and closed at
 * random, off every edge of the screen and on top of one another, with
 * a frame's damage now and then past what it keeps. Through gfx.library's
 * table, wrapped to check that a frame writes no pixel twice and never
 * reads the screen; the scalar entries, and on an Arm host NEON's too.
 * Some windows have chrome.c's chrome around them, as parts of their own,
 * in System Sans or the 8x8 font; some are resized, with a flat part
 * past their buffer; a pinned window stays in front of them all, and one
 * kept behind (WFLG_BACKDROP's, V8 step 7) behind them all.
 *
 * Then chrome.c alone: what's where on a window, and that drawing it
 * touches only its bitmaps; bar.c's screen bar, menus and requesters
 * likewise, each written to build/intuition/ as a picture to look at.
 * And events.c: a client reading slower than intuition writes, at
 * random, must get every key and button, in order, and the last of each
 * run of moves. font.library is lib/font, linked in (gfx_host_ifont).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/font.h>

#include "../../lib/gfx/rows.h"
#include "bar.h"
#include "chrome.h"
#include "compose.h"
#include "events.h"

extern const struct FontIFace *gfx_host_ifont;

/* Areas: the host's memory, page aligned and zeroed (the fonts' caches). */
static void *areas[16];

int64_t AllocArea(uint64_t size, uint32_t, void **address)
{
    size = (size + 4095) & ~4095ull;
    for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++)
        if (!areas[i]) {
            if (!(areas[i] = aligned_alloc(4096, size)))
                return ERR_NOMEM;
            memset(areas[i], 0, size);
            *address = areas[i];
            return i + 1;
        }
    return ERR_NOMEM;
}

int64_t FreeArea(Handle h)
{
    if (h < 1 || h > sizeof areas / sizeof areas[0] || !areas[h - 1])
        return ERR_BADHANDLE;
    free(areas[h - 1]);
    areas[h - 1] = nullptr;
    return 0;
}

int64_t QueryArea(const void *, struct AreaInfo *)
{
    return ERR_NOTFOUND;
}

constexpr uint32_t W = 200, H = 150;
constexpr unsigned WINDOWS = 8, FRAMES = 3000, SEEDS = 4;
constexpr uint32_t POISON = 0xdeadbeef;

static uint64_t seed;

static uint32_t rnd(uint32_t n)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (uint32_t)(seed % n);
}

static int32_t between(int32_t lo, int32_t hi)
{
    return lo + (int32_t)rnd((uint32_t)(hi - lo + 1));
}

/* --- gfx.library's table, watched. --- */

static const struct GfxIFace *real;
static struct BitMap *target;
static uint8_t writes[H][W];
static const char *fault;

static void mark(const struct RastPort *rp, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    if (rp->rp_BitMap != target) {
        fault = "drew somewhere other than the screen";
        return;
    }
    for (int64_t y = y0 < 0 ? 0 : y0; y <= y1 && y < H; y++)
        for (int64_t x = x0 < 0 ? 0 : x0; x <= x1 && x < W; x++)
            if (++writes[y][x] > 1)
                fault = "a pixel written twice in a frame";
}

static void w_fill(struct RastPort *rp, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    mark(rp, x0, y0, x1, y1);
    real->RectFill(rp, x0, y0, x1, y1);
}

static void w_blt(const struct BitMap *src, int32_t sx, int32_t sy, struct RastPort *dst, int32_t dx,
                  int32_t dy, int32_t w, int32_t h, uint32_t blend)
{
    if (src == target)
        fault = "read the screen";
    if (blend != BLEND_SRC)
        fault = "blended an opaque window";
    if (sx < 0 || sy < 0 || (uint32_t)(sx + w) > src->bm_Width || (uint32_t)(sy + h) > src->bm_Height)
        fault = "blitted from outside a window's bitmap";
    mark(dst, dx, dy, (int64_t)dx + w - 1, (int64_t)dy + h - 1);
    real->BltBitMap(src, sx, sy, dst, dx, dy, w, h, blend);
}

#define TABLE(fill, blt, text, path, mask, fpath, grad, run) { \
    .AllocBitMap = AllocBitMap, .FreeBitMap = FreeBitMap, .InitBitMap = InitBitMap, \
    .InitRastPort = InitRastPort, .RectFill = fill, .BltBitMap = blt, .DrawBevel = DrawBevel, \
    .Text = text, .PathBounds = PathBounds, .PathMaskScratch = PathMaskScratch, .PathMask = path, \
    .BltMask = mask, .FillPath = fpath, .FillGradient = grad, .PathRoundRect = PathRoundRect, \
    .InitTmpRas = InitTmpRas, .OpenTextFont = OpenTextFont, .CloseTextFont = CloseTextFont, \
    .SetFont = SetFont, .TextLength = TextLength, .TextRun = run }

static const struct GfxIFace scalar =
    TABLE(RectFill, BltBitMap, Text, PathMask, BltMask, FillPath, FillGradient, TextRun);

#ifdef GFX_TEST_NEON
static const struct GfxIFace neon = TABLE(RectFillVector, BltBitMapVector, TextVector, PathMaskVector,
                                          BltMaskVector, FillPathVector, FillGradientVector, TextRunVector);
#endif

static struct chrome_fonts fonts;       /* System Sans 13, regular and bold */

static struct GfxIFace watched = { .InitBitMap = InitBitMap, .InitRastPort = InitRastPort,
                                   .RectFill = w_fill, .BltBitMap = w_blt };

/* --- The windows, as a client keeps them. --- */

struct win {
    bool open, pinned, back, grown;
    struct ic_window icw;
    struct chrome ch;
    int32_t ix, iy;
    uint32_t iw, ih;
    struct BitMap bm[2], top, side;
    uint32_t *px[2], *tpx, *spx;
    unsigned cur;
};

static struct win wins[WINDOWS];

static uint32_t *at(struct BitMap *bm, int32_t x, int32_t y)
{
    return (uint32_t *)((uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow) + x;
}

static void scribble(struct BitMap *bm, struct GfxRect r)
{
    uint32_t c = (uint32_t)rnd(0x1000000) | rnd(256) << 24;

    for (int32_t y = r.MinY; y <= r.MaxY; y++)
        for (int32_t x = r.MinX; x <= r.MaxX; x++)
            *at(bm, x, y) = c ^ (uint32_t)(x * 7 + y * 131);
}

/* Some of v's inside, in its own coordinates. */
static struct GfxRect some_of(const struct win *v)
{
    int32_t x0 = between(0, (int32_t)v->iw - 1), y0 = between(0, (int32_t)v->ih - 1);

    return (struct GfxRect){ x0, y0, between(x0, (int32_t)v->iw - 1), between(y0, (int32_t)v->ih - 1) };
}

static uint32_t *own(struct BitMap *bm, uint32_t w, uint32_t h)
{
    uint32_t *px = malloc((uint64_t)w * h * 4);

    real->InitBitMap(bm, px, w, h, w * 4, PIXFMT_XRGB8888);
    return px;
}

static void open_win(struct ic_screen *s, struct win *v)
{
    uint32_t w = (uint32_t)between(1, 120), h = (uint32_t)between(1, 90);
    uint32_t flags = rnd(2) ? WFLG_BORDERLESS : (rnd(2) ? WFLG_DEPTHGADGET : 0) | (rnd(2) ? WFLG_CLOSEGADGET : 0);

    for (unsigned b = 0; b < 2; b++) {
        /* Buffers a little bigger than the window, with rows to spare. */
        uint32_t bw = w + rnd(4), bh = h + rnd(4), bpr = (bw + rnd(3)) * 4;

        v->px[b] = malloc((uint64_t)bpr * bh);
        real->InitBitMap(&v->bm[b], v->px[b], bw, bh, bpr, rnd(2) ? PIXFMT_ARGB8888 : PIXFMT_XRGB8888);
        scribble(&v->bm[b], (struct GfxRect){ 0, 0, (int32_t)bw - 1, (int32_t)bh - 1 });
    }
    v->cur = 0;
    v->grown = false;
    v->iw = w;
    v->ih = h;
    chrome_size(flags, w, h, &v->ch, &v->ix, &v->iy);
    v->ch.active = rnd(2);
    v->ch.title = "a window";
    v->ch.fonts = rnd(4) ? &fonts : nullptr;
    v->tpx = own(&v->top, v->ch.w, INTUI_TITLEBAR + chrome_bottom(flags));
    v->spx = own(&v->side, 2 * INTUI_BORDER, h);
    chrome_draw(real, &v->top, &v->side, &v->ch);
    v->icw = (struct ic_window){ between(-60, W + 10), between(-50, H + 10), v->ch.w, v->ch.h, 1, { 0 }, 0 };
    v->icw.part[0] = (struct ic_part){ { v->ix, v->iy, v->ix + (int32_t)w - 1, v->iy + (int32_t)h - 1 },
                                       &v->bm[0], 0, 0, 0 };
    chrome_parts(&v->ch, &v->top, &v->side, &v->icw);
    v->pinned = v == &wins[0];          /* the first: always in front, as the screen bar is */
    v->back = v == &wins[WINDOWS - 1];  /* the last: always behind, as the Workbench's backdrop is */
    v->open = v->pinned ? ic_pin(s, &v->icw) : v->back ? ic_add_back(s, &v->icw) : ic_add(s, &v->icw);
}

/* Made bigger, as a resize leaves a window until its client gives new
 * buffers: the buffer shown where it reaches, a flat part past it. */
static void grow(struct ic_screen *s, struct win *v)
{
    uint32_t dw = rnd(20), dh = rnd(20), bw = v->bm[v->cur].bm_Width, bh = v->bm[v->cur].bm_Height;
    int32_t x1, y1, sw, sh;
    unsigned n;

    if (!(v->ch.flags & WFLG_BORDERLESS) || v->pinned)
        return;                         /* only bare ones: the chrome's bitmaps are their size */
    v->grown = true;
    v->iw += dw;
    v->ih += dh;
    v->ch.w += dw;
    v->ch.h += dh;
    ic_resize(s, &v->icw, v->ch.w, v->ch.h);
    sw = (int32_t)(bw < v->iw ? bw : v->iw);
    sh = (int32_t)(bh < v->ih ? bh : v->ih);
    x1 = (int32_t)v->iw - 1, y1 = (int32_t)v->ih - 1;
    v->icw.part[0].r = (struct GfxRect){ 0, 0, sw - 1, sh - 1 };
    n = 1;
    if (sw <= x1)
        v->icw.part[n++] = (struct ic_part){ { sw, 0, x1, y1 }, nullptr, 0, 0, 0xff102030 };
    if (sh <= y1)
        v->icw.part[n++] = (struct ic_part){ { 0, sh, sw - 1, y1 }, nullptr, 0, 0, 0xff302010 };
    v->icw.nparts = n;
}

static void close_win(struct ic_screen *s, struct win *v)
{
    ic_remove(s, &v->icw);
    free(v->px[0]);
    free(v->px[1]);
    free(v->tpx);
    free(v->spx);
    v->open = false;
}

/* A frame of v's: in the other buffer brought up to date, or the same
 * one, with some rectangles changed and said to be. */
static void present(struct ic_screen *s, struct win *v)
{
    unsigned n = rnd(8) == 0 ? IC_DAMAGE + 4 : 1 + rnd(3);
    struct BitMap *b;

    if (rnd(2) && !v->grown) {         /* a grown window keeps its buffer */
        unsigned other = v->cur ^ 1;

        for (uint32_t y = 0; y < v->ih; y++)
            memcpy(at(&v->bm[other], 0, (int32_t)y), at(&v->bm[v->cur], 0, (int32_t)y), v->iw * 4);
        v->cur = other;
    }
    b = &v->bm[v->cur];
    v->icw.part[0].bm = b;
    for (unsigned i = 0; i < n; i++) {
        struct GfxRect r = some_of(v);

        if (r.MaxX >= (int32_t)b->bm_Width || r.MaxY >= (int32_t)b->bm_Height)
            continue;                   /* past its buffer, grown */

        if (n > IC_DAMAGE)              /* specks, mostly apart: more than a frame keeps */
            r.MaxX = r.MinX, r.MaxY = r.MinY;

        scribble(b, r);
        ic_damage_window(s, &v->icw, (struct GfxRect){ v->ix + r.MinX, v->iy + r.MinY,
                                                       v->ix + r.MaxX, v->iy + r.MaxY });
    }
}

/* Its chrome drawn again, active or not: its parts but the inside damaged. */
static void refocus(struct ic_screen *s, struct win *v)
{
    v->ch.active = !v->ch.active;
    v->ch.pressed = rnd(3) == 0 ? CH_CLOSE : CH_NONE;
    chrome_draw(real, &v->top, &v->side, &v->ch);
    for (unsigned k = 1; k < v->icw.nparts; k++)
        ic_damage_window(s, &v->icw, v->icw.part[k].r);
}

/* --- The naive compositor. --- */

static uint32_t naive(const struct ic_screen *s, int32_t x, int32_t y)
{
    for (unsigned z = 0; z < s->nz; z++) {
        const struct ic_window *w = s->z[z];

        if (x >= w->x && y >= w->y && x < (int64_t)w->x + w->w && y < (int64_t)w->y + w->h) {
            int32_t wx = x - w->x, wy = y - w->y;

            for (unsigned k = 0; k < w->nparts; k++) {
                const struct ic_part *p = &w->part[k];

                if (wx >= p->r.MinX && wx <= p->r.MaxX && wy >= p->r.MinY && wy <= p->r.MaxY)
                    return p->bm ? *at((struct BitMap *)p->bm, p->sx + wx - p->r.MinX, p->sy + wy - p->r.MinY)
                                 : p->colour;
            }
            return POISON ^ 1;          /* a hole in the parts: can't match */
        }
    }
    return s->backdrop;
}

static int run(const char *name, const struct GfxIFace *g)
{
    static uint32_t screen[2][H][W];
    struct BitMap sb[2];
    struct ic_screen s;
    unsigned frames = 0, back = 0;
    uint64_t painted = 0;

    real = g;
    for (unsigned k = 0; k < SEEDS; k++) {
        seed = 0x9e3779b97f4a7c15ull * (k + 1);
        for (unsigned b = 0; b < 2; b++) {
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++)
                    screen[b][y][x] = POISON;
            g->InitBitMap(&sb[b], screen[b], W, H, W * 4, PIXFMT_XRGB8888);
        }
        ic_init(&s, &watched, W, H);
        for (unsigned f = 0; f < FRAMES; f++) {
            unsigned ops = 1 + rnd(3);

            for (unsigned o = 0; o < ops && f > 0; o++) {
                struct win *v = &wins[rnd(WINDOWS)];

                if (!v->open) {
                    open_win(&s, v);
                    continue;
                }
                switch (rnd(7)) {
                case 0:
                    close_win(&s, v);
                    break;
                case 1:
                    ic_move(&s, &v->icw, between(-60, W + 10), between(-50, H + 10));
                    break;
                case 2:         /* raised, or lowered */
                    if (rnd(2))
                        ic_raise(&s, &v->icw);
                    else
                        ic_lower(&s, &v->icw);
                    if (s.z[0] != &wins[0].icw && wins[0].open)
                        fault = "a window raised past a pinned one";
                    break;
                case 5:
                    if (rnd(4) == 0)
                        grow(&s, v);
                    else
                        present(&s, v);
                    break;
                case 6:
                    refocus(&s, v);
                    break;
                case 3:
                    ic_move(&s, &v->icw, v->icw.x + between(-3, 3), v->icw.y + between(-3, 3));
                    present(&s, v);
                    break;
                default:
                    present(&s, v);
                }
            }
            if (wins[WINDOWS - 1].open && s.z[s.nz - 1] != &wins[WINDOWS - 1].icw)
                fault = "a window in behind the one kept behind";
            if (ic_dirty(&s)) {
                uint64_t was = s.painted, pending = ic_pending(&s);

                memset(writes, 0, sizeof writes);
                target = &sb[back];
                ic_compose(&s, target);
                if (s.painted - was != pending)
                    fault = "ic_pending wasn't what the frame painted";
                painted += s.painted - was;
                frames++;
                back ^= 1;
            }
            if (fault) {
                printf("intuition-test: FAILED: %s: %s, seed %u frame %u\n", name, fault, k, f);
                return 1;
            }
            /* What the screen shows: the buffer composed last. */
            for (int32_t y = 0; y < (int32_t)H; y++)
                for (int32_t x = 0; x < (int32_t)W; x++)
                    if (screen[back ^ 1][y][x] != naive(&s, x, y)) {
                        printf("intuition-test: FAILED: %s: (%d, %d) is %08x, not %08x, seed %u "
                               "frame %u, %u windows\n",
                               name, x, y, screen[back ^ 1][y][x], naive(&s, x, y), k, f, s.nz);
                        return 1;
                    }
        }
        for (unsigned i = 0; i < WINDOWS; i++)
            if (wins[i].open)
                close_win(&s, &wins[i]);
    }
    printf("intuition-test: ok: %s: %u frames as the naive compositor draws them, %.1f%% of the "
           "screen repainted a frame, no pixel twice\n",
           name, frames, 100.0 * (double)painted / frames / (W * H));
    return 0;
}

/* --- chrome.c alone. --- */

static int chrome_test(void)
{
    static const struct { int32_t x, y; unsigned part; } where[] = {
        { 0, 0, CH_TITLE }, { 50, 10, CH_TITLE }, { 4, 26, CH_INSIDE }, { 203, 145, CH_INSIDE },
        { 3, 26, CH_FRAME }, { 204, 100, CH_FRAME }, { 100, 146, CH_FRAME }, { 100, 25, CH_TITLE },
        { 203, 3, CH_CLOSE }, { 182, 22, CH_CLOSE }, { 181, 10, CH_TITLE }, { 179, 10, CH_DEPTH },
        { 158, 3, CH_DEPTH }, { 157, 3, CH_TITLE }, { 208, 0, CH_NONE }, { -1, 5, CH_NONE },
    };
    struct chrome c = { .title = "a title much too long to fit in the bar beside its gadgets" };
    struct BitMap top, side;
    uint32_t *tp, *sp;
    int32_t ix, iy;

    real = &scalar;
    c.fonts = &fonts;
    chrome_size(WFLG_CLOSEGADGET | WFLG_DEPTHGADGET | WFLG_DRAGBAR, 200, 120, &c, &ix, &iy);
    if (c.w != 208 || c.h != 150 || ix != 4 || iy != 26) {
        printf("intuition-test: FAILED: a window of 200x120 is %ux%u, its inside at (%d, %d)\n", c.w,
               c.h, ix, iy);
        return 1;
    }
    for (unsigned i = 0; i < sizeof where / sizeof where[0]; i++)
        if (chrome_hit(&c, where[i].x, where[i].y) != where[i].part) {
            printf("intuition-test: FAILED: (%d, %d) of a window is %u, not %u\n", where[i].x,
                   where[i].y, chrome_hit(&c, where[i].x, where[i].y), where[i].part);
            return 1;
        }
    c.flags = WFLG_BORDERLESS;
    if (chrome_hit(&c, 0, 0) != CH_INSIDE) {
        printf("intuition-test: FAILED: a borderless window has chrome\n");
        return 1;
    }
    c.flags = WFLG_DEPTHGADGET;                     /* no close gadget: depth's in its slot */
    if (chrome_hit(&c, 203, 3) != CH_DEPTH || chrome_hit(&c, 179, 10) != CH_TITLE) {
        printf("intuition-test: FAILED: the depth gadget isn't rightmost without the close one\n");
        return 1;
    }
    chrome_size(WFLG_SIZEGADGET | WFLG_CLOSEGADGET, 200, 120, &c, &ix, &iy);
    if (c.h != 120 + INTUI_TITLEBAR + INTUI_SIZEBORDER || chrome_hit(&c, 200, 155) != CH_SIZE ||
        chrome_hit(&c, 100, 155) != CH_FRAME || chrome_hit(&c, 203, 145) != CH_INSIDE ||
        chrome_hit(&c, 190, 155) != CH_FRAME) {
        printf("intuition-test: FAILED: the size gadget isn't in the bottom right corner\n");
        return 1;
    }

    /* Drawn, in bitmaps with a row and a column to spare, poisoned. */
    tp = malloc((208 + 1) * (30 + 1) * 4);
    sp = malloc((8 + 1) * (120 + 1) * 4);
    for (unsigned i = 0; i < (208 + 1) * (30 + 1); i++)
        tp[i] = POISON;
    for (unsigned i = 0; i < (8 + 1) * (120 + 1); i++)
        sp[i] = POISON;
    InitBitMap(&top, tp, 208, 30, (208 + 1) * 4, PIXFMT_XRGB8888);
    InitBitMap(&side, sp, 8, 120, (8 + 1) * 4, PIXFMT_XRGB8888);
    c.flags = WFLG_CLOSEGADGET | WFLG_DEPTHGADGET;
    c.active = true;
    chrome_draw(&scalar, &top, &side, &c);
    for (unsigned y = 0; y < 31; y++)
        for (unsigned x = 0; x < 209; x++)
            if ((tp[y * 209 + x] == POISON) != (x == 208 || y == 30)) {
                printf("intuition-test: FAILED: the title bar's bitmap at (%u, %u) %s\n", x, y,
                       x == 208 || y == 30 ? "drawn past its edge" : "not drawn");
                return 1;
            }
    for (unsigned y = 0; y < 121; y++)
        for (unsigned x = 0; x < 9; x++)
            if ((sp[y * 9 + x] == POISON) != (x == 8 || y == 120)) {
                printf("intuition-test: FAILED: the sides' bitmap at (%u, %u) %s\n", x, y,
                       x == 8 || y == 120 ? "drawn past its edge" : "not drawn");
                return 1;
            }
    if (tp[0] != 0xff162030 || tp[1 * 209 + 1] != 0xff8bb4e8 || sp[50 * 9 + 3] != 0xff162030 ||
        sp[50 * 9 + 4] != 0xff8bb4e8) {
        printf("intuition-test: FAILED: the frame's bevel isn't where it should be\n");
        return 1;
    }
    free(tp);
    free(sp);
    printf("intuition-test: ok: the chrome: what's where, and drawn in its bitmaps alone\n");
    return 0;
}

/* --- bar.c: the screen bar, a menu and a requester. --- */

/* A bitmap w by h with a row and a column to spare, poisoned. */
static uint32_t *poisoned(struct BitMap *bm, uint32_t w, uint32_t h)
{
    uint32_t *px = malloc((uint64_t)(w + 1) * (h + 1) * 4);

    for (uint64_t i = 0; i < (uint64_t)(w + 1) * (h + 1); i++)
        px[i] = POISON;
    InitBitMap(bm, px, w, h, (w + 1) * 4, PIXFMT_XRGB8888);
    return px;
}

/* Every pixel of it drawn, and none past it. */
static bool drawn(const uint32_t *px, uint32_t w, uint32_t h, const char *what)
{
    for (uint32_t y = 0; y <= h; y++)
        for (uint32_t x = 0; x <= w; x++)
            if ((px[y * (w + 1) + x] == POISON) != (x == w || y == h)) {
                printf("intuition-test: FAILED: %s at (%u, %u) %s\n", what, x, y,
                       x == w || y == h ? "drawn past its edge" : "not drawn");
                return false;
            }
    return true;
}

/* A picture to look at: build/intuition/<name>.ppm. */
static void picture(const char *name, const struct BitMap *bm)
{
    char path[128];
    FILE *f;

    snprintf(path, sizeof path, "build/intuition/%s.ppm", name);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n%u %u\n255\n", bm->bm_Width, bm->bm_Height);
    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            uint32_t p = *at((struct BitMap *)bm, (int32_t)x, (int32_t)y);
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };

            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static struct IntuiMenuStrip strip;

static void strip_init(void)
{
    static const struct { uint8_t menu; const char *label; char key; uint16_t flags; } items[] = {
        { 0, "Open\xe2\x80\xa6", 'O', 0 }, { 0, "Save", 'S', 0 }, { 0, nullptr, 0, ITEMBAR },
        { 0, "Quit", 'Q', 0 }, { 1, "Snap to grid", 0, CHECKIT | CHECKED }, { 1, "Wrap lines", 0, CHECKIT },
        { 1, "Print", 'P', NM_ITEMDISABLED }, { 2, "Help", 0, 0 },
    };

    memset(&strip, 0, sizeof strip);
    strcpy(strip.ms_Title[0], "Project");
    strcpy(strip.ms_Title[1], "Options");
    strcpy(strip.ms_Title[2], "Help");
    strip.ms_Menus = 3;
    strip.ms_Disabled = 1u << 2;
    for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++) {
        struct IntuiMenuItem *mi = &strip.ms_Item[strip.ms_Items++];

        mi->mi_Menu = items[i].menu;
        mi->mi_Key = items[i].key;
        mi->mi_Flags = items[i].flags;
        if (items[i].label)
            strcpy(mi->mi_Label, items[i].label);
    }
}

static int bar_test(void)
{
    static const struct { uint64_t s; const char *want; } times[] = {
        { 0, "12:00 AM" }, { 59, "12:00 AM" }, { 13 * 3600 + 5 * 60, "1:05 PM" }, { 23 * 3600 + 59 * 60 + 59, "11:59 PM" },
        { 12 * 3600, "12:00 PM" }, { 1761480000, "12:00 PM" }, { 10 * 3600 + 38 * 60, "10:38 AM" },
    };
    struct bar b = { .width = 640, .fonts = &fonts, .strip = &strip, .open = -1 };
    struct menu m = { .strip = &strip, .menu = 0, .hot = -1 };
    struct request q = { 0 };
    struct BitMap bm;
    uint32_t *px;
    char clock[12];

    real = &scalar;
    strip_init();
    for (unsigned i = 0; i < sizeof times / sizeof times[0]; i++) {
        bar_clock(clock, times[i].s);
        if (strcmp(clock, times[i].want)) {
            printf("intuition-test: FAILED: %llu s into a day is \"%s\", not \"%s\"\n",
                   (unsigned long long)times[i].s, clock, times[i].want);
            return 1;
        }
    }

    /* The bar: the system menu's title, then the strip's, apart, left to right. */
    strcpy(b.clock, "12:38 PM");
    bar_layout(&scalar, &b);
    if (b.n != 4 || b.x0[0] != 4 || bar_hit(&b, 10, 10) != 0 || bar_hit(&b, 10, 28) != -1 ||
        bar_hit(&b, 639, 10) != -1) {
        printf("intuition-test: FAILED: the bar's titles: %u, the first from %d\n", b.n, b.x0[0]);
        return 1;
    }
    for (unsigned i = 1; i < b.n; i++)
        if (b.x0[i] <= b.x1[i - 1] || b.x1[i] <= b.x0[i] || bar_hit(&b, (b.x0[i] + b.x1[i]) / 2, 14) != (int)i) {
            printf("intuition-test: FAILED: the bar's title %u at %d to %d\n", i, b.x0[i], b.x1[i]);
            return 1;
        }
    px = poisoned(&bm, 640, INTUI_SCREENBAR);
    b.open = 1;
    bar_draw(&scalar, &bm, &b);
    if (!drawn(px, 640, INTUI_SCREENBAR, "the screen bar"))
        return 1;
    picture("bar", &bm);
    free(px);

    /* A menu: its items one under another, a separator and a disabled
     * one not to be chosen. */
    menu_layout(&scalar, &fonts, &m);
    if (m.count != 4 || m.first != 0 || m.w < 120 || menu_hit(&m, 10, m.y[1] + 2) != 1 ||
        menu_hit(&m, (int32_t)m.w, m.y[1] + 2) != -1 || menu_hit(&m, 10, 0) != -1 ||
        !menu_choosable(&m, 1) || menu_choosable(&m, 2) || m.y[3] - m.y[2] >= m.y[2] - m.y[1]) {
        printf("intuition-test: FAILED: a menu's items: %u, %ux%u\n", m.count, m.w, m.h);
        return 1;
    }
    m.hot = 1;
    px = poisoned(&bm, m.w, m.h);
    menu_draw(&scalar, &fonts, &bm, &m);
    if (!drawn(px, m.w, m.h, "a menu"))
        return 1;
    picture("menu", &bm);
    free(px);
    m = (struct menu){ .strip = &strip, .menu = 1, .hot = 0 };
    menu_layout(&scalar, &fonts, &m);
    if (m.count != 3 || m.first != 4 || menu_choosable(&m, 2)) {
        printf("intuition-test: FAILED: the second menu's items\n");
        return 1;
    }
    m = (struct menu){ .strip = &strip, .menu = 2, .hot = -1 };
    menu_layout(&scalar, &fonts, &m);
    if (m.count != 1 || menu_choosable(&m, 0)) {
        printf("intuition-test: FAILED: a disabled menu's item can be chosen\n");
        return 1;
    }

    /* A requester: two lines, two buttons, the first at the left. */
    strcpy(q.r.ir_Title, "Program not answering");
    strcpy(q.r.ir_Body, "The program isn't answering.\nClose it?");
    strcpy(q.r.ir_Gadgets, "Close|Wait");
    if (!request_layout(&scalar, &fonts, &q, 600) || q.nlines != 2 || q.nbuttons != 2 ||
        q.button[0].MaxX >= q.button[1].MinX || q.button[0].MinX != 16 ||
        q.button[1].MaxX != (int32_t)q.w - 17 ||
        request_hit(&q, (q.button[0].MinX + q.button[0].MaxX) / 2, q.button[0].MinY + 3) != 0 ||
        request_hit(&q, (q.button[1].MinX + q.button[1].MaxX) / 2, q.button[1].MaxY) != 1 ||
        request_hit(&q, 2, 2) != -1 || request_answer(&q, 0) != 1 || request_answer(&q, 1) != 0) {
        printf("intuition-test: FAILED: a requester: %u lines, %u buttons, %ux%u\n", q.nlines, q.nbuttons, q.w, q.h);
        return 1;
    }
    q.pressed = 0;
    px = poisoned(&bm, q.w, q.h);
    request_draw(&scalar, &fonts, &bm, &q);
    if (!drawn(px, q.w, q.h, "a requester"))
        return 1;
    picture("request", &bm);
    free(px);
    strcpy(q.r.ir_Gadgets, "");
    if (request_layout(&scalar, &fonts, &q, 600)) {
        printf("intuition-test: FAILED: a requester with no buttons\n");
        return 1;
    }
    printf("intuition-test: ok: the screen bar, a menu and a requester: where things are, and drawn "
           "in their bitmaps alone (build/intuition/*.ppm)\n");
    return 0;
}

/* The desktop composed, to look at: the bar, a window with a menu open
 * over it, and a requester (build/intuition/desktop.ppm). */
static int desktop(void)
{
    constexpr uint32_t SW = 640, SH = 400;
    static uint32_t screen[SH][SW];
    struct BitMap sb, top, side, inside, barbm, menubm, rtop, rside, rbody;
    struct ic_window bw, ww, mw, rw;
    struct ic_screen s;
    struct chrome c = { .title = "Workbench", .active = true, .fonts = &fonts }, rc = { 0 };
    struct bar b = { .width = SW, .fonts = &fonts, .strip = &strip, .open = 1 };
    struct menu m = { .strip = &strip, .menu = 0, .hot = 1 };
    struct request q = { 0 };
    struct RastPort rp;
    void *mem[9];
    int32_t ix, iy, rix, riy;
    unsigned n = 0;

    real = &scalar;
    InitBitMap(&sb, screen, SW, SH, SW * 4, PIXFMT_XRGB8888);
    ic_init(&s, &scalar, SW, SH);
    strcpy(b.clock, "12:38 PM");
    bar_layout(&scalar, &b);
    mem[n++] = own(&barbm, SW, INTUI_SCREENBAR);
    bar_draw(&scalar, &barbm, &b);
    bw = (struct ic_window){ 0, 0, SW, INTUI_SCREENBAR, 1, { { { 0, 0, SW - 1, INTUI_SCREENBAR - 1 }, &barbm, 0, 0, 0 } }, 0 };
    ic_pin(&s, &bw);

    chrome_size(WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_SIZEGADGET, 360, 200, &c, &ix, &iy);
    mem[n++] = own(&top, c.w, INTUI_TITLEBAR + INTUI_SIZEBORDER);
    mem[n++] = own(&side, 2 * INTUI_BORDER, 200);
    mem[n++] = own(&inside, 360, 200);
    chrome_draw(&scalar, &top, &side, &c);
    InitRastPort(&rp, &inside);
    FillGradient(&rp, &(struct GfxGradient){ { 0, 0, 359, 199 }, 0xff36435a, 0xff46556e, 0 }, nullptr, 0);
    ww = (struct ic_window){ 20, 60, c.w, c.h, 1, { { { ix, iy, ix + 359, iy + 199 }, &inside, 0, 0, 0 } }, 0 };
    chrome_parts(&c, &top, &side, &ww);
    ic_add(&s, &ww);

    menu_layout(&scalar, &fonts, &m);
    mem[n++] = own(&menubm, m.w, m.h);
    menu_draw(&scalar, &fonts, &menubm, &m);
    mw = (struct ic_window){ b.x0[1], INTUI_SCREENBAR, m.w, m.h, 1, { { { 0, 0, (int32_t)m.w - 1, (int32_t)m.h - 1 }, &menubm, 0, 0, 0 } }, 0 };
    ic_pin(&s, &mw);

    strcpy(q.r.ir_Title, "Program not answering");
    strcpy(q.r.ir_Body, "The program isn't answering.\nClose it?");
    strcpy(q.r.ir_Gadgets, "Close|Wait");
    request_layout(&scalar, &fonts, &q, SW - 40);
    rc.title = q.r.ir_Title;
    rc.active = true;
    rc.fonts = &fonts;
    chrome_size(0, q.w, q.h, &rc, &rix, &riy);
    mem[n++] = own(&rtop, rc.w, INTUI_TITLEBAR + INTUI_BORDER);
    mem[n++] = own(&rside, 2 * INTUI_BORDER, q.h);
    mem[n++] = own(&rbody, q.w, q.h);
    chrome_draw(&scalar, &rtop, &rside, &rc);
    request_draw(&scalar, &fonts, &rbody, &q);
    rw = (struct ic_window){ 390, 230, rc.w, rc.h, 1, { { { rix, riy, rix + (int32_t)q.w - 1, riy + (int32_t)q.h - 1 }, &rbody, 0, 0, 0 } }, 0 };
    chrome_parts(&rc, &rtop, &rside, &rw);
    ic_add(&s, &rw);
    ic_compose(&s, &sb);
    picture("desktop", &sb);
    for (unsigned i = 0; i < n; i++)
        free(mem[i]);
    return 0;
}

/* --- events.c: a client slower than intuition. --- */

static int events_test(void)
{
    static struct IntuiPage page;
    struct ie_queue q = { 0 };
    uint64_t tail = 0, sent = 0, got = 0, moves = 0, lost_moves = 0;
    uint32_t want = 0;                  /* the next key or button's number */
    int32_t last_x = -1, seen_x = -1;

    seed = 12345;
    for (unsigned round = 0; round < 200000; round++) {
        /* intuition: a burst of moves now and then, and keys. */
        unsigned n = rnd(10) == 0 ? 1 + rnd(200) : rnd(3);

        for (unsigned i = 0; i < n; i++) {
            struct IntuiEvent e = { .iev_Window = 1 };

            if (rnd(4) || sent - got >= IE_KEPT + INTUI_EVENTS_RING - INTUI_EVENTS_MOVES) {
                e.iev_Class = IDCMP_MOUSEMOVE;
                e.iev_MouseX = ++last_x;
                moves++;
            } else {
                e.iev_Class = rnd(2) ? IDCMP_RAWKEY : IDCMP_MOUSEBUTTONS;
                e.iev_Char = (uint32_t)sent++;
            }
            ie_push(&q, &e, &page.ip_Lost);
            ie_flush(&q, &page, tail);
        }
        /* The client: some of what's there, when it gets round to it. */
        for (unsigned k = rnd(3) ? rnd(4) : rnd(150); k && tail < page.ip_Head; k--, tail++) {
            const struct IntuiEvent *e = &page.ip_Event[tail % INTUI_EVENTS_RING];

            if (e->iev_Class == IDCMP_MOUSEMOVE) {
                if (e->iev_MouseX <= seen_x) {
                    printf("intuition-test: FAILED: events: a move came out of order\n");
                    return 1;
                }
                seen_x = e->iev_MouseX;
                continue;
            }
            if (e->iev_Char != want) {
                printf("intuition-test: FAILED: events: key or button %u came, not %u\n",
                       e->iev_Char, want);
                return 1;
            }
            want++;
            got++;
        }
        ie_flush(&q, &page, tail);
    }
    while (tail < page.ip_Head || q.n) {        /* the rest, read to the end */
        const struct IntuiEvent *e = &page.ip_Event[tail % INTUI_EVENTS_RING];

        if (tail == page.ip_Head) {
            ie_flush(&q, &page, tail);
            continue;
        }
        if (e->iev_Class == IDCMP_MOUSEMOVE)
            seen_x = e->iev_MouseX;
        else if (e->iev_Char == want++)
            got++;
        tail++;
    }
    lost_moves = page.ip_Lost;
    if (got != sent || seen_x != last_x) {
        printf("intuition-test: FAILED: events: %lu of %lu keys and buttons, the last move %d not %d\n",
               (unsigned long)got, (unsigned long)sent, seen_x, last_x);
        return 1;
    }
    printf("intuition-test: ok: events: %lu keys and buttons all came, in order, to a slow client; "
           "%lu moves, %lu dropped as it fell behind, the last one kept\n",
           (unsigned long)sent, (unsigned long)moves, (unsigned long)lost_moves);
    return 0;
}

/* ic_start: a frame put off until just before the vblank, or not. */
static int pace_test(void)
{
    constexpr uint64_t P = 16666667, LEAD = 4000000, V = 1000000000;
    static const struct {
        uint64_t now, cost;
        bool waited;
        uint64_t want;
    } cases[] = {
        { V + 1000000, 0, false, V + P - LEAD },            /* the pointer alone */
        { V + 1000000, 100000, false, V + P - LEAD - 100000 },  /* a drag, the CIX's cost */
        { V + 1000000, 2400000, false, V + P - LEAD - 2400000 },
        { V + 1000000, 100000, true, 0 },                   /* a client waits */
        { V + P - LEAD - 99999, 100000, false, 0 },         /* already late */
        { V + P + 1000, 0, false, 0 },                      /* past the next vblank */
        { V + 1000000, P / 2, false, 0 },                   /* too costly to put off */
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (ic_start(cases[i].now, V, P, LEAD, cases[i].cost, cases[i].waited) != cases[i].want) {
            printf("intuition-test: FAILED: pace: case %u\n", i);
            return 1;
        }
    printf("intuition-test: ok: pace: a frame nobody waits for put off to its lead before the vblank\n");
    return 0;
}

int main(void)
{
    static const struct FontIFace font = {
        .SystemFont = SystemFont, .OpenFont = OpenFont, .FontInfo = FontInfo, .FontName = FontName,
        .MapCodepoint = MapCodepoint, .GetGlyphMetrics = GetGlyphMetrics, .GlyphOutline = GlyphOutline,
        .ScaleUnits = ScaleUnits, .ShapeRun = ShapeRun, .FitLine = FitLine, .LineSpacing = LineSpacing,
    };

    gfx_host_ifont = &font;
    fonts.sans = OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 });
    fonts.bold = OpenTextFont(&(struct TextAttr){ "System Sans", 13, FSF_BOLD, 0 });
    if (!fonts.sans || !fonts.bold) {
        printf("intuition-test: FAILED: System Sans wouldn't open\n");
        return 1;
    }
    if (run("scalar", &scalar))
        return 1;
#ifdef GFX_TEST_NEON
    if (run("NEON", &neon))
        return 1;
#endif
    if (chrome_test() || bar_test() || desktop() || events_test() || pace_test())
        return 1;
    CloseTextFont((struct TextFont *)fonts.sans);
    CloseTextFont((struct TextFont *)fonts.bold);
    return 0;
}
