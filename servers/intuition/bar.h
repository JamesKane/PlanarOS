/*
 * servers/intuition/bar.h -- the screen bar, its menus and requesters
 * (V8 step 4)
 *
 * What intuition draws of its own besides the chrome, each into a bitmap
 * of its own with gfx.library, in §13.2's colours and System Sans:
 *
 * - The screen bar (§13.3): the top INTUI_SCREENBAR rows, the screen's
 *   width. The Planar mark and "PlanarOS" in bold at the left, which
 *   open the system menu; the active window's menu titles after them;
 *   the clock at the right. A menu open shows its title highlighted.
 * - A menu: a panel of its items under its title, one highlighted when
 *   the pointer is on it, a separator a groove, a disabled item muted,
 *   a CHECKIT item a check when it's checked, a key at the right.
 * - A requester's inside (EasyRequest): its lines of text, and its
 *   buttons along the bottom, one drawn pressed.
 *
 * Each has a layout, which says where its parts are; a draw, from it;
 * and a hit, what's at a point. Plain C over gfx.library's table, tested
 * on the host.
 */
#ifndef SERVERS_INTUITION_BAR_H
#define SERVERS_INTUITION_BAR_H

#include <stdint.h>

#include <planar/intuition.h>

#include "chrome.h"

/* --- The screen bar. --- */

constexpr unsigned BAR_TITLES = 1 + INTUI_MENU_TITLES;     /* the system menu's, then the strip's */

struct bar {
    uint32_t width;
    const struct chrome_fonts *fonts;
    const struct IntuiMenuStrip *strip; /* the active window's; nullptr, none */
    char clock[12];                     /* "12:38 PM", or "" */
    int open;                           /* the title open: 0 the system menu's, 1 + n the strip's n; -1 none */
    /* bar_layout's: each title's span, the system menu's first. */
    unsigned n;
    int32_t x0[BAR_TITLES], x1[BAR_TITLES];
};

void bar_layout(const struct GfxIFace *g, struct bar *b);
void bar_draw(const struct GfxIFace *g, struct BitMap *bm, const struct bar *b);

/* The title at (x, y) of the bar, or -1. */
int bar_hit(const struct bar *b, int32_t x, int32_t y);

/* The time of day 's' seconds into a day (UTC's, the RTC's), as the
 * clock shows it. */
void bar_clock(char out[12], uint64_t s);

/* --- A menu. --- */

struct menu {
    const struct IntuiMenuStrip *strip;
    unsigned menu;                      /* which of its menus */
    unsigned first, count;              /* its items in the strip */
    int hot;                            /* the item highlighted, of this menu's; -1 none */
    uint32_t w, h;                      /* menu_layout's: the panel's size */
    int32_t y[INTUI_MENU_ITEMS + 1];    /* each item's top in it, and the last's bottom */
};

void menu_layout(const struct GfxIFace *g, const struct chrome_fonts *f, struct menu *m);
void menu_draw(const struct GfxIFace *g, const struct chrome_fonts *f, struct BitMap *bm,
               const struct menu *m);

/* The item at (x, y) of the panel, or -1; and whether it can be chosen. */
int menu_hit(const struct menu *m, int32_t x, int32_t y);
bool menu_choosable(const struct menu *m, int item);

/* --- A requester's inside. --- */

constexpr unsigned REQ_LINES = 12, REQ_BUTTONS = 6;

struct request {
    struct IntuiRequest r;              /* intuition's copy */
    unsigned nlines, nbuttons;
    const char *line[REQ_LINES];        /* in r */
    uint32_t len[REQ_LINES];
    const char *label[REQ_BUTTONS];
    uint32_t llen[REQ_BUTTONS];
    struct GfxRect button[REQ_BUTTONS]; /* in the inside */
    uint32_t w, h;                      /* the inside's */
    int pressed;                        /* the button drawn pressed; -1 none */
};

/* Laid out from q->r, at most max_w wide: false if it has no buttons. */
bool request_layout(const struct GfxIFace *g, const struct chrome_fonts *f, struct request *q,
                    uint32_t max_w);
void request_draw(const struct GfxIFace *g, const struct chrome_fonts *f, struct BitMap *bm,
                  const struct request *q);

/* The button at (x, y) of the inside, or -1; and what choosing it
 * answers: 1 to n - 1 from the left, 0 the rightmost (the RKM's). */
int request_hit(const struct request *q, int32_t x, int32_t y);
int request_answer(const struct request *q, int button);

#endif /* SERVERS_INTUITION_BAR_H */
