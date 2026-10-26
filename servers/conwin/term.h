/*
 * servers/conwin/term.h -- a console window's text: the terminal, its
 * scrollback, and the line being typed (V8 step 5)
 *
 * What conwin shows, apart from conwin: plain C over gfx.library's
 * table, tested on the host (test.c).
 *
 * - The lines: a ring of TERM_LINES lines of TERM_COLS cells, the
 *   newest at the bottom of the window, the cursor on the newest. A
 *   line written past the window's width goes on at the start of the
 *   next (it wraps as it's written); a resize reflows nothing, so lines
 *   written wider are cut at the window's edge, as a terminal cuts them.
 * - What's written: UTF-8, LF a new line (and column 0, as the Amiga's
 *   CON: has it), CR column 0, BS a column back, TAB to the next of every
 *   8th (or the last), FF the window cleared. CSI (ESC [, or 0x9b) with SGR's m (0
 *   plain, 1 bold, 22 not, 30 to 37 and 90 to 97 a colour, 39 the
 *   text's), K the line cleared from the cursor, J (2J) the window
 *   cleared. Other sequences are read and dropped.
 * - The view: scrolled back by keys (term_scroll) into the scrollback,
 *   and back to the bottom by any key typed.
 * - Typing (cooked, as CON: is): a line is edited here, echoed as it's
 *   typed, and queued for reading when Return ends it. BS deletes,
 *   Ctrl-U (or Ctrl-X) the whole line, Ctrl-\ is the end of the input.
 *   A read takes what's queued (term_read).
 * - Drawing: the rows changed since the last draw, each whole, in
 *   System Mono on pane.terminal.bg, a glyph a cell; the cursor a block
 *   (an outline when the window isn't active).
 */
#ifndef SERVERS_CONWIN_TERM_H
#define SERVERS_CONWIN_TERM_H

#include <stdint.h>

#include <planar/gfx.h>

constexpr unsigned TERM_COLS = 200;     /* a line's cells, at most */
constexpr unsigned TERM_LINES = 1000;   /* lines kept, the window's among them */
constexpr unsigned TERM_ROWS = 128;     /* the window's rows, at most */
constexpr unsigned TERM_INPUT = 256;    /* a line being typed, its bytes */
constexpr unsigned TERM_QUEUE = 1024;   /* bytes typed and not yet read */
constexpr int32_t TERM_PAD = 4;         /* around the cells, inside the window */

/* §13.2's: pane.terminal.bg, text.light, text.shell.prompt. */
constexpr uint32_t TERM_BG = 0xff121822, TERM_FG = 0xfff0f4fa, TERM_PROMPT = 0xff4ade80;

struct term_cell {
    uint16_t ch;                        /* a code point, U+0000 to U+FFFF; 0 blank */
    uint8_t pen;                        /* 0 the text's, 1 + n SGR's colour n (0 to 15) */
    uint8_t bold;
};

struct term_font {
    const struct TextFont *regular, *bold;
    uint32_t cw, ch;                    /* a cell's size, pixels */
};

struct term {
    struct term_cell *cell;             /* TERM_LINES * TERM_COLS: the caller's */
    uint32_t cols, rows;                /* the window's, in cells */
    uint64_t lines;                     /* lines ever begun: the cursor's is lines - 1 */
    uint64_t home;                      /* the line the last clear began; the window's top till it fills */
    uint32_t cx;                        /* the cursor's column; may be cols (a wrap to come) */
    uint32_t view;                      /* lines scrolled back from the bottom */
    uint8_t pen, bold;                  /* SGR's, for what's written next */
    /* An escape being read. */
    uint8_t esc;                        /* 0 none, 1 after ESC, 2 in CSI */
    uint8_t nparam;
    uint16_t param[4];
    /* UTF-8 being read. */
    uint32_t cp;
    uint8_t need;
    /* The rows to draw again, of the window's; and all of them. */
    uint8_t dirty[TERM_ROWS];
    bool all;
    bool active;                        /* the window's: the cursor's look */
    uint64_t drawn_line, drawn_top;     /* the cursor's line and the top row's, as last drawn */
    uint32_t drawn_col;
    /* The line being typed, and what's queued for reading. */
    char in[TERM_INPUT];
    uint32_t inlen;
    char queue[TERM_QUEUE];
    uint32_t qhead, qlen;
    bool eof;                           /* Ctrl-\ typed: a read now answers nothing */
};

/* Empty, cols by rows, over 'cells' (TERM_LINES * TERM_COLS of them). */
void term_init(struct term *t, struct term_cell *cells, uint32_t cols, uint32_t rows);
/* The window's new size, in cells: nothing reflowed. */
void term_resize(struct term *t, uint32_t cols, uint32_t rows);
void term_write(struct term *t, const char *text, uint32_t len);
/* A key typed: a character (1 to 0x10ffff). */
void term_key(struct term *t, uint32_t ch);
/* Back into the scrollback by 'lines' (forward if less than 0), as far as it goes. */
void term_scroll(struct term *t, int32_t lines);
/* What's queued for reading, up to 'len' bytes: how many. */
uint32_t term_read(struct term *t, char *buf, uint32_t len);
void term_set_active(struct term *t, bool active);

/* The cell at row r, column c of the window, as the view shows it. */
const struct term_cell *term_at(const struct term *t, uint32_t r, uint32_t c);
/* The colour of a pen. */
uint32_t term_pen(unsigned pen);
/* f's cells: as wide as a digit of 'regular' (rounded), 18 px high (§13.2). */
void term_font_init(const struct GfxIFace *g, struct term_font *f, const struct TextFont *regular,
                    const struct TextFont *bold);
/* The window's inside for cols by rows of f's cells. */
uint32_t term_width(const struct term_font *f, uint32_t cols);
uint32_t term_height(const struct term_font *f, uint32_t rows);

/* The rows changed since the last draw, into rp (the window's inside,
 * width by height), and nothing more. The rectangle drawn, in *drawn;
 * false if nothing was. */
bool term_draw(const struct GfxIFace *g, const struct term_font *f, struct RastPort *rp, uint32_t width,
               uint32_t height, struct term *t, struct GfxRect *drawn);

#endif /* SERVERS_CONWIN_TERM_H */
