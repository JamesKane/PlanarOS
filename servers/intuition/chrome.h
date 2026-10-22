/*
 * servers/intuition/chrome.h -- a window's chrome (v7-plan step 4)
 *
 * §13.3's frame, drawn by intuition with gfx.library into two bitmaps
 * of its own for each window: 'top', the window's width by
 * INTUI_TITLEBAR + INTUI_BORDER, holding the title bar above and the
 * bottom of the frame below it; and 'side', 2 * INTUI_BORDER by the
 * inside's height, holding the frame's left and right. compose.c shows
 * them as four of the window's parts (chrome_parts), around its inside.
 * The gadgets are 22 by 20 at the title bar's right, the close one
 * rightmost. Plain C over gfx.library's table, tested on the host.
 */
#ifndef SERVERS_INTUITION_CHROME_H
#define SERVERS_INTUITION_CHROME_H

#include <stdint.h>

#include <planar/intuition.h>

#include "compose.h"

/* What's at a point of a window. */
enum : unsigned { CH_NONE, CH_INSIDE, CH_TITLE, CH_DEPTH, CH_CLOSE, CH_FRAME };

struct chrome {
    uint32_t w, h;                      /* the window's, chrome and all */
    uint32_t flags;                     /* WFLG_* */
    bool active;
    unsigned pressed;                   /* CH_DEPTH or CH_CLOSE, drawn pressed; or CH_NONE */
    const char *title;
};

/* The window's size and where its inside is, for an inside of iw by ih. */
void chrome_size(uint32_t flags, uint32_t iw, uint32_t ih, struct chrome *c, int32_t *ix,
                 int32_t *iy);

/* The chrome drawn in 'top' and 'side', the sizes above. */
void chrome_draw(const struct GfxIFace *g, struct BitMap *top, struct BitMap *side,
                 const struct chrome *c);

/* The four parts of 'w' that show them, after its inside's (part[0]). */
void chrome_parts(const struct chrome *c, const struct BitMap *top, const struct BitMap *side,
                  struct ic_window *w);

/* What's at (x, y) of the window, in its own coordinates. */
unsigned chrome_hit(const struct chrome *c, int32_t x, int32_t y);

#endif /* SERVERS_INTUITION_CHROME_H */
