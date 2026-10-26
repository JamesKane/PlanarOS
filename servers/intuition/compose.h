/*
 * servers/intuition/compose.h -- the screen as intuition composes it
 * (v7-plan steps 3 and 4)
 *
 * compose.c keeps the windows in depth order, front first, and what has
 * changed on the screen since the last frame. A frame is composed into
 * one of two screen buffers in turn, so the one being drawn is a frame
 * old: each frame repaints what changed for it and for the last. Every
 * pixel repainted is written once, from the front window over it or the
 * backdrop, and nothing is read from the screen (windows are opaque in
 * V7, decision 3). A window is a rectangle made of parts, each from a
 * bitmap or a flat colour: its inside from its client's buffer, and its
 * chrome from intuition's own. The first few windows can be pinned at
 * the front (V8 step 4: the screen bar, an open menu, a resize's
 * outline), where no other window is raised past them. It's plain C
 * over gfx.library's table: main.c gives it IGfx, and test.c lib/gfx's
 * scalar entries on the host.
 */
#ifndef SERVERS_INTUITION_COMPOSE_H
#define SERVERS_INTUITION_COMPOSE_H

#include <stdint.h>

#include <planar/gfx.h>

constexpr unsigned IC_WINDOWS = 32;         /* on the screen, every client's */
constexpr unsigned IC_DAMAGE = 16;          /* rectangles a frame keeps, before merging */
constexpr uint32_t IC_BACKDROP = 0xff182038;    /* bg.backdrop.base's first stop (§13.2) */

/* A window's: the inside, the title bar and 3 sides, and 2 beyond a
 * buffer smaller than the inside (a window resized, not yet given new
 * buffers). */
constexpr unsigned IC_PARTS = 7;

struct ic_part {
    struct GfxRect r;               /* of the window, in its own coordinates */
    const struct BitMap *bm;        /* r's pixels: bm's from (sx, sy); nullptr, 'colour' */
    int32_t sx, sy;
    uint32_t colour;
};

struct ic_window {
    int32_t x, y;                   /* its top left on the screen */
    uint32_t w, h;
    unsigned nparts;                /* 0: nothing to show yet */
    struct ic_part part[IC_PARTS];  /* apart, and covering all of it */
    uint32_t kind;                  /* the owner's: compose.c doesn't look */
};

/* Rectangles on the screen, inclusive, clipped to it; disjoint once
 * added, overlapping ones merged into their bounds. */
struct ic_damage {
    unsigned n;
    struct GfxRect r[IC_DAMAGE];
};

struct ic_screen {
    const struct GfxIFace *gfx;
    uint32_t width, height;
    uint32_t backdrop;
    struct ic_window *z[IC_WINDOWS];    /* front first */
    unsigned nz;
    unsigned pinned;                /* z's first, kept in front */
    unsigned backs;                 /* z's last, kept behind (V8 step 7: WFLG_BACKDROP) */
    struct ic_damage now;           /* since the last frame */
    struct ic_damage last;          /* the last frame's */
    uint64_t painted;               /* pixels written, ever */
};

/* A screen with no windows, all of it to paint in each buffer. */
void ic_init(struct ic_screen *s, const struct GfxIFace *gfx, uint32_t width, uint32_t height);

/* A rectangle of the screen changed. */
void ic_damage(struct ic_screen *s, struct GfxRect r);

/* A rectangle of window 'w' changed, in its own coordinates. */
void ic_damage_window(struct ic_screen *s, const struct ic_window *w, struct GfxRect r);

/* 'w' to the front, behind the pinned windows, or out: its rectangle
 * damaged. ic_add is false if the screen has IC_WINDOWS already. ic_pin
 * puts it in front of them all, pinned; ic_add_back behind them all,
 * kept there, in front of those kept there before. */
bool ic_add(struct ic_screen *s, struct ic_window *w);
bool ic_pin(struct ic_screen *s, struct ic_window *w);
bool ic_add_back(struct ic_screen *s, struct ic_window *w);
void ic_remove(struct ic_screen *s, struct ic_window *w);

/* 'w', on the screen and not pinned, to the front (behind the pinned) or
 * the back (in front of those kept behind): its rectangle damaged. One
 * kept behind moves among those alone. */
void ic_raise(struct ic_screen *s, struct ic_window *w);
void ic_lower(struct ic_screen *s, struct ic_window *w);

/* The front and the back of the windows neither pinned nor kept behind,
 * or nullptr. */
struct ic_window *ic_front(const struct ic_screen *s);
struct ic_window *ic_back(const struct ic_screen *s);

/* The front window at (x, y) of the screen, or nullptr. */
struct ic_window *ic_at(const struct ic_screen *s, int32_t x, int32_t y);

/* 'w' moved to (x, y): where it was and where it is, damaged. */
void ic_move(struct ic_screen *s, struct ic_window *w, int32_t x, int32_t y);

/* 'w' made w by h, its parts the caller's to set: where it was and
 * where it is, damaged. */
void ic_resize(struct ic_screen *s, struct ic_window *w, uint32_t width, uint32_t height);

bool ic_dirty(const struct ic_screen *s);

/* The pixels the next frame will paint. */
uint64_t ic_pending(const struct ic_screen *s);

/*
 * When to start composing and committing a frame, 0 at once (V7 step
 * 6). One a client waits for is at once. One nobody waits for (the
 * pointer moved, a window dragged, chrome redrawn) is put off until
 * 'lead' plus 'cost', what composing it should take, before the vblank
 * after 'vblank': a window's frame presented meanwhile then goes in the
 * same commit, where one committed first would hold it to the latch
 * after. A cost of half a period or more is at once.
 */
uint64_t ic_start(uint64_t now, uint64_t vblank, uint64_t period, uint64_t lead, uint64_t cost,
                  bool waited);

/* The next frame into 'target', the screen's size: what changed for it
 * and for the last, repainted. */
void ic_compose(struct ic_screen *s, struct BitMap *target);

#endif /* SERVERS_INTUITION_COMPOSE_H */
