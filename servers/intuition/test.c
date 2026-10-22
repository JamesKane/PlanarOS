/*
 * servers/intuition/test.c -- composition on the host (make intuition-test)
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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../lib/gfx/rows.h"
#include "compose.h"

int64_t AllocArea(uint64_t, uint32_t, void **)
{
    return ERR_NOMEM;
}

int64_t FreeArea(Handle)
{
    return ERR_BADHANDLE;
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

static const struct GfxIFace scalar = {
    .InitBitMap = InitBitMap, .InitRastPort = InitRastPort, .RectFill = RectFill,
    .BltBitMap = BltBitMap, .DrawBevel = DrawBevel, .Text = Text,
};

#ifdef GFX_TEST_NEON
static const struct GfxIFace neon = {
    .InitBitMap = InitBitMap, .InitRastPort = InitRastPort, .RectFill = RectFillVector,
    .BltBitMap = BltBitMapVector, .DrawBevel = DrawBevelVector, .Text = Text,
};
#endif

static struct GfxIFace watched = { .InitBitMap = InitBitMap, .InitRastPort = InitRastPort,
                                   .RectFill = w_fill, .BltBitMap = w_blt };

/* --- The windows, as a client keeps them. --- */

struct win {
    bool open;
    struct ic_window icw;
    struct BitMap bm[2];
    uint32_t *px[2];
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

static struct GfxRect some_of(const struct win *v)
{
    int32_t x0 = between(0, (int32_t)v->icw.w - 1), y0 = between(0, (int32_t)v->icw.h - 1);

    return (struct GfxRect){ x0, y0, between(x0, (int32_t)v->icw.w - 1), between(y0, (int32_t)v->icw.h - 1) };
}

static void open_win(struct ic_screen *s, struct win *v)
{
    uint32_t w = (uint32_t)between(1, 120), h = (uint32_t)between(1, 90);

    for (unsigned b = 0; b < 2; b++) {
        /* Buffers a little bigger than the window, with rows to spare. */
        uint32_t bw = w + rnd(4), bh = h + rnd(4), bpr = (bw + rnd(3)) * 4;

        v->px[b] = malloc((uint64_t)bpr * bh);
        real->InitBitMap(&v->bm[b], v->px[b], bw, bh, bpr, rnd(2) ? PIXFMT_ARGB8888 : PIXFMT_XRGB8888);
        scribble(&v->bm[b], (struct GfxRect){ 0, 0, (int32_t)bw - 1, (int32_t)bh - 1 });
    }
    v->cur = 0;
    v->icw = (struct ic_window){ between(-60, W + 10), between(-50, H + 10), w, h, &v->bm[0] };
    v->open = ic_add(s, &v->icw);
}

static void close_win(struct ic_screen *s, struct win *v)
{
    ic_remove(s, &v->icw);
    free(v->px[0]);
    free(v->px[1]);
    v->open = false;
}

/* A frame of v's: in the other buffer brought up to date, or the same
 * one, with some rectangles changed and said to be. */
static void present(struct ic_screen *s, struct win *v)
{
    unsigned n = rnd(8) == 0 ? IC_DAMAGE + 4 : 1 + rnd(3);
    struct BitMap *b;

    if (rnd(2)) {
        unsigned other = v->cur ^ 1;

        for (uint32_t y = 0; y < v->icw.h; y++)
            memcpy(at(&v->bm[other], 0, (int32_t)y), at(&v->bm[v->cur], 0, (int32_t)y), v->icw.w * 4);
        v->cur = other;
    }
    b = &v->bm[v->cur];
    v->icw.bm = b;
    for (unsigned i = 0; i < n; i++) {
        struct GfxRect r = some_of(v);

        if (n > IC_DAMAGE)              /* specks, mostly apart: more than a frame keeps */
            r.MaxX = r.MinX, r.MaxY = r.MinY;

        scribble(b, r);
        ic_damage_window(s, &v->icw, r);
    }
}

/* --- The naive compositor. --- */

static uint32_t naive(const struct ic_screen *s, int32_t x, int32_t y)
{
    for (unsigned z = 0; z < s->nz; z++) {
        const struct ic_window *w = s->z[z];

        if (x >= w->x && y >= w->y && x < (int64_t)w->x + w->w && y < (int64_t)w->y + w->h)
            return *at((struct BitMap *)w->bm, x - w->x, y - w->y);
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
                switch (rnd(6)) {
                case 0:
                    close_win(&s, v);
                    break;
                case 1:
                    ic_move(&s, &v->icw, between(-60, W + 10), between(-50, H + 10));
                    break;
                case 2:         /* raised: out and in again, at the front */
                    ic_remove(&s, &v->icw);
                    ic_add(&s, &v->icw);
                    break;
                case 3:
                    ic_move(&s, &v->icw, v->icw.x + between(-3, 3), v->icw.y + between(-3, 3));
                    present(&s, v);
                    break;
                default:
                    present(&s, v);
                }
            }
            if (ic_dirty(&s)) {
                uint64_t was = s.painted;

                memset(writes, 0, sizeof writes);
                target = &sb[back];
                ic_compose(&s, target);
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

int main(void)
{
    if (run("scalar", &scalar))
        return 1;
#ifdef GFX_TEST_NEON
    if (run("NEON", &neon))
        return 1;
#endif
    return 0;
}
