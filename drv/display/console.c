/*
 * drv/display/console.c -- the text console, on plane 0 (V4 step 7)
 *
 * Text in the 8x8 font, twice as big on screens 1600 pixels wide or more,
 * scrolling up a line at a time; the rest of the screen is the
 * background. What's drawn is cleaned from the caches after each write,
 * for a controller that doesn't snoop them (both boards'), and told to
 * the backend, which on fb.c composes it with the planes above. Each
 * line also goes to the kernel's log, and so to the serial console
 * (V4 decision 1: the console is both).
 *
 * A restarted display (DRVF_AGAIN) finds the last one's text still on
 * the screen, and goes on below it: the screen doesn't blank.
 */
#include <planar/syscalls.h>

#include "display.h"
#include "font8x8.h"

constexpr uint32_t FG = 0xe8e8e8;           /* light grey on dark slate blue, omega's */
constexpr uint32_t BG = 0x203050;
constexpr unsigned LINE = 160;

static struct {
    uint32_t scale, cell, cols, rows, col, row;
    char line[LINE];                        /* for the log */
    unsigned nline;
    uint32_t dirty_lo, dirty_hi;            /* rows changed since the last flush */
    bool owed;                              /* a newline, before the next text */
} con;

/* A colour as 0xRRGGBB, as the console's memory holds it. */
static uint32_t pixel(const struct dp_out *o, uint32_t rgb)
{
    return o->con_bgr ? rgb : (rgb & 0xff) << 16 | (rgb & 0xff00) | rgb >> 16;
}

static void dirty(const struct dp_out *o, uint32_t y0, uint32_t y1)
{
    if (y0 < con.dirty_lo)
        con.dirty_lo = y0;
    if (y1 > con.dirty_hi)
        con.dirty_hi = y1 < o->con_height ? y1 : o->con_height;
}

/* The rows drawn, out of the caches to memory, and to the backend. */
static void flush(struct dp_out *o)
{
    if (con.dirty_hi > con.dirty_lo) {
        dp_clean((const volatile void *)(o->con_fb + (uint64_t)con.dirty_lo * o->con_stride),
                 (uint64_t)(con.dirty_hi - con.dirty_lo) * o->con_stride * 4);
        if (o->be->damage)
            o->be->damage(o, con.dirty_lo, con.dirty_hi);
    }
    con.dirty_lo = UINT32_MAX;
    con.dirty_hi = 0;
}

static void fill(struct dp_out *o, uint32_t y0, uint32_t y1, uint32_t rgb)
{
    uint32_t p = pixel(o, rgb);

    dirty(o, y0, y1);
    for (uint32_t y = y0; y < y1 && y < o->con_height; y++)
        for (uint32_t x = 0; x < o->con_width; x++)
            o->con_fb[(uint64_t)y * o->con_stride + x] = p;
}

static void glyph(struct dp_out *o, char ch)
{
    const uint8_t *g = font8x8[0];
    uint32_t fg = pixel(o, FG), bg = pixel(o, BG);
    uint32_t x0 = con.col * con.cell, y0 = con.row * con.cell;

    if ((unsigned char)ch >= FONT8X8_FIRST && (unsigned char)ch < FONT8X8_FIRST + FONT8X8_COUNT)
        g = font8x8[(unsigned char)ch - FONT8X8_FIRST];
    dirty(o, y0, y0 + con.cell);
    for (uint32_t y = 0; y < con.cell; y++)
        for (uint32_t x = 0; x < con.cell; x++)
            o->con_fb[(uint64_t)(y0 + y) * o->con_stride + x0 + x] =
                g[y / con.scale] >> (x / con.scale) & 1 ? fg : bg;
}

static void scroll(struct dp_out *o)
{
    uint32_t keep = (con.rows - 1) * con.cell;

    dirty(o, 0, con.rows * con.cell);
    for (uint32_t y = 0; y < keep; y++)
        for (uint32_t x = 0; x < con.cols * con.cell; x++)
            o->con_fb[(uint64_t)y * o->con_stride + x] =
                o->con_fb[(uint64_t)(y + con.cell) * o->con_stride + x];
    fill(o, keep, con.rows * con.cell, BG);
}

static void newline(struct dp_out *o)
{
    con.col = 0;
    if (++con.row == con.rows) {
        scroll(o);
        con.row = con.rows - 1;
    }
}

static void log_line(void)
{
    con.line[con.nline] = '\0';
    KLog(con.line);
    con.nline = 0;
}

static void put(struct dp_out *o, char ch)
{
    if (con.owed) {
        con.owed = false;
        newline(o);
    }
    if (ch == '\n') {
        newline(o);
        log_line();
        return;
    }
    if (ch == '\r')
        return;
    if (ch == '\b') {
        if (con.col)
            con.col--;
        if (con.nline)
            con.nline--;
        glyph(o, ' ');
        return;
    }
    if (ch == '\t')
        ch = ' ';
    glyph(o, ch);
    if (++con.col == con.cols)
        newline(o);
    if (con.nline < LINE - 1)
        con.line[con.nline++] = ch;
}

static void say(struct dp_out *o, const char *s)
{
    for (; *s; s++, con.col++)
        glyph(o, *s);
    newline(o);
}

void con_start(struct dp_out *o, bool again)
{
    con.scale = o->con_width >= 1600 ? 2 : 1;
    con.cell = 8 * con.scale;
    con.cols = o->con_width / con.cell;
    con.rows = o->con_height / con.cell;
    con.dirty_lo = UINT32_MAX;
    con.dirty_hi = 0;
    if (again) {
        /* The last one's text stays; ours goes on over its bottom line,
         * without a scroll, which reads the whole screen. */
        con.row = con.rows - 1;
        fill(o, con.row * con.cell, (con.row + 1) * con.cell, BG);
        for (const char *s = "display restarted"; *s; s++, con.col++)
            glyph(o, *s);
        con.owed = true;
    } else {
        fill(o, 0, o->con_height, BG);
        say(o, "Planar");
        newline(o);
    }
    flush(o);
}

void con_write(struct dp_out *o, const uint8_t *text, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        put(o, (char)text[i]);
    flush(o);
}

bool con_fits(const struct dp_out *o)
{
    return o->con_width / 8 >= 20 && o->con_height / 8 >= 4;
}
