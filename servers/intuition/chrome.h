/*
 * servers/intuition/chrome.h -- a window's chrome (v7-plan step 4, V8
 * step 4)
 *
 * §13.3's frame, drawn by intuition with gfx.library into two bitmaps
 * of its own for each window: 'top', the window's width by
 * INTUI_TITLEBAR + its bottom border, holding the title bar above and
 * the bottom of the frame below it; and 'side', 2 * INTUI_BORDER by the
 * inside's height, holding the frame's left and right. compose.c shows
 * them as four of the window's parts (chrome_parts), around its inside.
 * The gadgets are 22 by 20 at the title bar's right, the close one
 * rightmost; a window with WFLG_SIZEGADGET has a bottom border
 * INTUI_SIZEBORDER tall and the size gadget at its right end. The title
 * is in System Sans 13 px, bold for the active window, or the 8x8 font
 * if those wouldn't open. Plain C over gfx.library's table, tested on
 * the host.
 */
#ifndef SERVERS_INTUITION_CHROME_H
#define SERVERS_INTUITION_CHROME_H

#include <stdint.h>

#include <planar/intuition.h>

#include "compose.h"

/* What's at a point of a window. */
enum : unsigned { CH_NONE, CH_INSIDE, CH_TITLE, CH_DEPTH, CH_CLOSE, CH_SIZE, CH_FRAME };

/* §13.2's colours, intuition's: the screen bar, menus and requesters
 * have them too. */
constexpr uint32_t PEN_TEXT_LIGHT = 0xfff0f4fa, PEN_TEXT_MUTED = 0xff7e8ca3;
constexpr uint32_t PEN_TEXT_DARK = 0xff141824, PEN_SHADOW = 0xff0e131d;
constexpr uint32_t PEN_BEVEL_DARK = 0xff162030;

/* The fonts the chrome is drawn in: nullptr for the 8x8. */
struct chrome_fonts {
    const struct TextFont *sans, *bold;
};

struct chrome {
    uint32_t w, h;                      /* the window's, chrome and all */
    uint32_t flags;                     /* WFLG_* */
    bool active;
    unsigned pressed;                   /* CH_DEPTH or CH_CLOSE, drawn pressed; or CH_NONE */
    const char *title;
    const struct chrome_fonts *fonts;
};

/* The bottom border's height: INTUI_SIZEBORDER with a size gadget. */
uint32_t chrome_bottom(uint32_t flags);

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

/* Text drawn at (x, y), its line's top left, in 'font' and 'pen', with
 * §13.2's treatment: a shadow a pixel below in 'shadow' unless it's 0. */
void chrome_text(const struct GfxIFace *g, struct RastPort *rp, const struct TextFont *font,
                 int32_t x, int32_t y, const char *s, uint64_t n, uint32_t pen, uint32_t shadow);

/* A line of text's height in 'font', and its width. */
int32_t chrome_line(const struct TextFont *font);
int32_t chrome_width(const struct GfxIFace *g, const struct TextFont *font, const char *s, uint64_t n);

#endif /* SERVERS_INTUITION_CHROME_H */
