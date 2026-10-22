/*
 * servers/intuition/compose.h -- the screen as intuition composes it
 * (v7-plan step 3)
 *
 * compose.c keeps the windows in depth order, front first, and what has
 * changed on the screen since the last frame. A frame is composed into
 * one of two screen buffers in turn, so the one being drawn is a frame
 * old: each frame repaints what changed for it and for the last. Every
 * pixel repainted is written once, from the front window over it or the
 * backdrop, and nothing is read from the screen (windows are opaque in
 * V7, decision 3). It's plain C over gfx.library's table: main.c gives
 * it IGfx, and test.c lib/gfx's scalar entries on the host.
 */
#ifndef SERVERS_INTUITION_COMPOSE_H
#define SERVERS_INTUITION_COMPOSE_H

#include <stdint.h>

#include <planar/gfx.h>

constexpr unsigned IC_WINDOWS = 32;         /* on the screen, every client's */
constexpr unsigned IC_DAMAGE = 16;          /* rectangles a frame keeps, before merging */
constexpr uint32_t IC_BACKDROP = 0xff182038;    /* bg.backdrop.base's first stop (§13.2) */

struct ic_window {
    int32_t x, y;                   /* its top left on the screen */
    uint32_t w, h;
    const struct BitMap *bm;        /* what it shows: its top left w by h */
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

/* 'w' to the front, or out: its rectangle damaged. ic_add is false if
 * the screen has IC_WINDOWS already. */
bool ic_add(struct ic_screen *s, struct ic_window *w);
void ic_remove(struct ic_screen *s, struct ic_window *w);

/* 'w' moved to (x, y): where it was and where it is, damaged. */
void ic_move(struct ic_screen *s, struct ic_window *w, int32_t x, int32_t y);

bool ic_dirty(const struct ic_screen *s);

/* The next frame into 'target', the screen's size: what changed for it
 * and for the last, repainted. */
void ic_compose(struct ic_screen *s, struct BitMap *target);

#endif /* SERVERS_INTUITION_COMPOSE_H */
