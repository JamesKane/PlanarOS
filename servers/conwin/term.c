/*
 * servers/conwin/term.c -- a console window's text (term.h)
 *
 * Line n (of every line begun) is the ring's n % TERM_LINES. The window
 * shows from 'first': the newest lines once they fill it, else from
 * 'home', the line a clear began (0 before any). The view goes back from
 * there into what the ring still holds.
 */
#include <stddef.h>

#include "term.h"

static struct term_cell *line(struct term *t, uint64_t n)
{
    return t->cell + (n % TERM_LINES) * TERM_COLS;
}

/* The line the window's top row shows, scrolled back or not. */
static uint64_t first(const struct term *t)
{
    uint64_t f = t->lines > t->rows ? t->lines - t->rows : 0;

    return (f > t->home ? f : t->home) - t->view;
}

/* How far back the view can go: to the oldest line the ring holds. */
static uint32_t view_max(const struct term *t)
{
    uint64_t f = t->lines > t->rows ? t->lines - t->rows : 0, oldest = t->lines > TERM_LINES ? t->lines - TERM_LINES : 0;

    f = f > t->home ? f : t->home;
    return f > oldest ? (uint32_t)(f - oldest > UINT32_MAX ? UINT32_MAX : f - oldest) : 0;
}

static void mark(struct term *t, uint64_t n)
{
    uint64_t top = first(t);

    if (n >= top && n - top < t->rows)
        t->dirty[n - top] = 1;
}

static void new_line(struct term *t)
{
    t->lines++;
    memset(line(t, t->lines - 1), 0, TERM_COLS * sizeof(struct term_cell));
    t->cx = 0;
    if (t->view && t->view < view_max(t))
        t->view++;                      /* the view stays on what it shows */
    t->all = true;                      /* scrolled, or a row more shown */
}

static void put(struct term *t, uint32_t cp)
{
    if (t->cx >= t->cols)
        new_line(t);                    /* a wrap: the line goes on at the next */
    if (t->cx < TERM_COLS)
        line(t, t->lines - 1)[t->cx] = (struct term_cell){ (uint16_t)(cp > 0xffff ? 0xfffd : cp), t->pen, t->bold };
    t->cx++;
    mark(t, t->lines - 1);
}

/* The window cleared: the cursor's line, a new one, at its top. */
static void clear(struct term *t)
{
    new_line(t);
    t->home = t->lines - 1;
    t->view = 0;
    t->all = true;
}

static void erase_line(struct term *t, uint32_t from)
{
    struct term_cell *l = line(t, t->lines - 1);

    for (uint32_t c = from; c < TERM_COLS; c++)
        l[c] = (struct term_cell){ 0 };
    mark(t, t->lines - 1);
}

static void sgr(struct term *t, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned p = t->param[i];

        if (p == 0)
            t->pen = t->bold = 0;
        else if (p == 1)
            t->bold = 1;
        else if (p == 22)
            t->bold = 0;
        else if (p >= 30 && p <= 37)
            t->pen = (uint8_t)(1 + p - 30);
        else if (p >= 90 && p <= 97)
            t->pen = (uint8_t)(9 + p - 90);
        else if (p == 39)
            t->pen = 0;
    }
}

/* A CSI sequence's last byte. */
static void csi(struct term *t, uint8_t final)
{
    unsigned n = t->nparam + 1u;

    if (final == 'm')
        sgr(t, n);
    else if (final == 'K')
        erase_line(t, t->cx);
    else if (final == 'J' && t->param[0] == 2)
        clear(t);
    else if (final == 'J')
        erase_line(t, t->cx);
}

static void control(struct term *t, uint8_t b)
{
    switch (b) {
    case '\n':
        new_line(t);
        break;
    case '\r':
        t->cx = 0;
        mark(t, t->lines - 1);
        break;
    case '\b':
        if (t->cx)
            t->cx = (t->cx > t->cols ? t->cols : t->cx) - 1;
        mark(t, t->lines - 1);
        break;
    case '\t':
        if (t->cx < t->cols - 1)        /* to the next stop, or the last column */
            t->cx = (t->cx / 8 + 1) * 8 < t->cols - 1 ? (t->cx / 8 + 1) * 8 : t->cols - 1;
        mark(t, t->lines - 1);
        break;
    case '\f':
        clear(t);
        break;
    case 0x1b:
        t->esc = 1;
        break;
    default:
        break;                          /* the rest of C0, and DEL: nothing */
    }
}

static void byte(struct term *t, uint8_t b)
{
    if (t->esc == 1) {
        t->esc = b == '[' ? 2 : 0;      /* not CSI: the byte after ESC dropped with it */
        t->nparam = 0;
        t->param[0] = 0;
        return;
    }
    if (t->esc == 2) {
        if (b >= '0' && b <= '9') {
            uint16_t *p = &t->param[t->nparam];

            *p = *p > 999 ? 9999 : (uint16_t)(*p * 10 + (b - '0'));
        } else if (b == ';') {
            if (t->nparam < 3)
                t->param[++t->nparam] = 0;
        } else if (b >= 0x40 && b <= 0x7e) {
            csi(t, b);
            t->esc = 0;
        } else if (b < 0x20 || b > 0x7e) {
            t->esc = 0;                 /* not a sequence after all */
        }
        return;
    }
    if (t->need) {
        if ((b & 0xc0) == 0x80) {
            t->cp = t->cp << 6 | (b & 0x3f);
            if (--t->need == 0)
                put(t, t->cp >= 0xd800 && t->cp < 0xe000 ? 0xfffd : t->cp);
            return;
        }
        t->need = 0;
        put(t, 0xfffd);                 /* cut short: then b as itself */
    }
    if (b < 0x20 || b == 0x7f) {
        control(t, b);
    } else if (b < 0x80) {
        put(t, b);
    } else if (b == 0x9b) {
        t->esc = 2;                     /* the Amiga's CSI, a byte */
        t->nparam = 0;
        t->param[0] = 0;
    } else if (b >= 0xc2 && b <= 0xdf) {
        t->cp = b & 0x1f, t->need = 1;
    } else if (b >= 0xe0 && b <= 0xef) {
        t->cp = b & 0x0f, t->need = 2;
    } else if (b >= 0xf0 && b <= 0xf4) {
        t->cp = b & 0x07, t->need = 3;
    } else {
        put(t, 0xfffd);
    }
}

void term_init(struct term *t, struct term_cell *cells, uint32_t cols, uint32_t rows)
{
    memset(t, 0, sizeof *t);
    t->cell = cells;
    t->lines = 1;
    memset(line(t, 0), 0, TERM_COLS * sizeof(struct term_cell));
    term_resize(t, cols, rows);
    t->active = true;
    t->drawn_top = UINT64_MAX;          /* nothing drawn yet */
}

void term_resize(struct term *t, uint32_t cols, uint32_t rows)
{
    t->cols = cols < 1 ? 1 : cols > TERM_COLS ? TERM_COLS : cols;
    t->rows = rows < 1 ? 1 : rows > TERM_ROWS ? TERM_ROWS : rows;
    if (t->view > view_max(t))
        t->view = view_max(t);
    t->all = true;
}

void term_write(struct term *t, const char *text, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        byte(t, (uint8_t)text[i]);
}

static uint32_t utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* The typed line's last character, deleted, and from the window. */
static bool rub_out(struct term *t)
{
    if (!t->inlen)
        return false;
    do
        t->inlen--;
    while (t->inlen && (t->in[t->inlen] & 0xc0) == 0x80);
    if (t->cx) {
        t->cx = (t->cx > t->cols ? t->cols : t->cx) - 1;
        if (t->cx < TERM_COLS)
            line(t, t->lines - 1)[t->cx] = (struct term_cell){ 0 };
        mark(t, t->lines - 1);
    }
    return true;
}

void term_key(struct term *t, uint32_t ch)
{
    if (t->view) {
        t->view = 0;                    /* typing goes back to the bottom */
        t->all = true;
    }
    if (ch == '\r' || ch == '\n') {
        for (uint32_t i = 0; i <= t->inlen && t->qlen < TERM_QUEUE; i++)
            t->queue[(t->qhead + t->qlen++) % TERM_QUEUE] = i < t->inlen ? t->in[i] : '\n';
        t->inlen = 0;
        term_write(t, "\n", 1);
    } else if (ch == 8 || ch == 0x7f) {
        rub_out(t);
    } else if (ch == 0x15 || ch == 0x18) {
        while (rub_out(t))
            ;
    } else if (ch == 0x1c) {
        t->eof = true;
    } else if (ch >= 0x20 && ch <= 0x10ffff && (ch < 0xd800 || ch > 0xdfff)) {
        char b[4];
        uint32_t n = utf8(ch, b);

        if (t->inlen + n < TERM_INPUT - 1) {
            memcpy(t->in + t->inlen, b, n);
            t->inlen += n;
            term_write(t, b, n);
        }
    }
}

void term_scroll(struct term *t, int32_t lines)
{
    int64_t v = (int64_t)t->view + lines, max = view_max(t);

    v = v < 0 ? 0 : v > max ? max : v;
    if ((uint32_t)v != t->view) {
        t->view = (uint32_t)v;
        t->all = true;
    }
}

uint32_t term_read(struct term *t, char *buf, uint32_t len)
{
    uint32_t n = 0;

    for (; n < len && t->qlen; n++, t->qlen--) {
        buf[n] = t->queue[t->qhead];
        t->qhead = (t->qhead + 1) % TERM_QUEUE;
    }
    return n;
}

void term_set_active(struct term *t, bool active)
{
    if (t->active != active)
        mark(t, t->lines - 1);
    t->active = active;
}

const struct term_cell *term_at(const struct term *t, uint32_t r, uint32_t c)
{
    static const struct term_cell blank;
    uint64_t n = first(t) + r;

    if (n >= t->lines || c >= TERM_COLS || r >= t->rows)
        return &blank;
    return &t->cell[(n % TERM_LINES) * TERM_COLS + c];
}

uint32_t term_pen(unsigned pen)
{
    static const uint32_t pens[17] = {
        TERM_FG,
        0xff4a5668, 0xffe05252, TERM_PROMPT, 0xfff0c040, 0xff5b88c2, 0xffc678dd, 0xff56c8d8, 0xffd8dee8,
        0xff7e8ca3, 0xfff07070, 0xff86efac, 0xfff8d870, 0xff8bb4e8, 0xffd8a0f0, 0xff8be0ea, 0xfff0f4fa,
    };

    return pen < 17 ? pens[pen] : TERM_FG;
}

void term_font_init(const struct GfxIFace *g, struct term_font *f, const struct TextFont *regular,
                    const struct TextFont *bold)
{
    struct RastPort rp = { 0 };
    int32_t w;

    g->SetFont(&rp, regular);
    w = g->TextLength(&rp, "0", 1);
    *f = (struct term_font){ regular, bold, w > 0 ? (uint32_t)w : 8, 18 };
    if (regular && regular->tf_YSize > f->ch)
        f->ch = regular->tf_YSize;
}

uint32_t term_width(const struct term_font *f, uint32_t cols)
{
    return cols * f->cw + 2 * TERM_PAD;
}

uint32_t term_height(const struct term_font *f, uint32_t rows)
{
    return rows * f->ch + 2 * TERM_PAD;
}

bool term_draw(const struct GfxIFace *g, const struct term_font *f, struct RastPort *rp, uint32_t width,
               uint32_t height, struct term *t, struct GfxRect *drawn)
{
    uint64_t top = first(t), cursor = t->lines - 1;
    uint32_t ccol = t->cx >= t->cols ? t->cols - 1 : t->cx;
    int32_t lo = -1, hi = -1;

    /* Where the cursor was drawn, and is to be. */
    if (t->drawn_line != cursor || t->drawn_col != ccol || t->drawn_top != top) {
        uint64_t was = t->drawn_line;

        if (was >= t->drawn_top && was - t->drawn_top < t->rows && t->drawn_top == top)
            t->dirty[was - top] = 1;
        mark(t, cursor);
    }
    if (t->drawn_top != top)
        t->all = true;
    rp->rp_DrawMode = JAM1;
    if (t->all) {
        rp->rp_FgPen = TERM_BG;
        g->RectFill(rp, 0, 0, (int32_t)width - 1, (int32_t)height - 1);
    }
    for (uint32_t r = 0; r < t->rows; r++) {
        int32_t y = TERM_PAD + (int32_t)(r * f->ch);

        if (!t->all && !t->dirty[r])
            continue;
        t->dirty[r] = 0;
        if (y >= (int32_t)height)
            continue;
        lo = lo < 0 ? (int32_t)r : lo;
        hi = (int32_t)r;
        if (!t->all) {
            rp->rp_FgPen = TERM_BG;
            g->RectFill(rp, 0, y, (int32_t)width - 1, y + (int32_t)f->ch - 1);
        }
        for (uint32_t c = 0; c < t->cols && TERM_PAD + (c + 1) * f->cw <= width; c++) {
            const struct term_cell *k = term_at(t, r, c);
            int32_t x = TERM_PAD + (int32_t)(c * f->cw);
            bool here = top + r == cursor && c == ccol;
            uint32_t pen = term_pen(k->pen);

            if (here && t->active) {
                rp->rp_FgPen = TERM_FG;
                g->RectFill(rp, x, y, x + (int32_t)f->cw - 1, y + (int32_t)f->ch - 1);
                pen = TERM_BG;
            } else if (here) {
                rp->rp_FgPen = TERM_FG;
                g->RectFill(rp, x, y, x + (int32_t)f->cw - 1, y);
                g->RectFill(rp, x, y + (int32_t)f->ch - 1, x + (int32_t)f->cw - 1, y + (int32_t)f->ch - 1);
                g->RectFill(rp, x, y, x, y + (int32_t)f->ch - 1);
                g->RectFill(rp, x + (int32_t)f->cw - 1, y, x + (int32_t)f->cw - 1, y + (int32_t)f->ch - 1);
            }
            if (k->ch > ' ') {
                const struct TextFont *tf = k->bold && f->bold ? f->bold : f->regular;
                char b[4];

                rp->rp_FgPen = pen;
                g->SetFont(rp, tf);
                g->Text(rp, x, y + ((int32_t)f->ch - (int32_t)tf->tf_YSize) / 2, b, utf8(k->ch, b));
            }
        }
    }
    t->drawn_line = cursor;
    t->drawn_col = ccol;
    t->drawn_top = top;
    if (lo < 0 && !t->all)
        return false;
    if (t->all)
        *drawn = (struct GfxRect){ 0, 0, (int32_t)width - 1, (int32_t)height - 1 };
    else
        *drawn = (struct GfxRect){ 0, TERM_PAD + lo * (int32_t)f->ch, (int32_t)width - 1,
                                   TERM_PAD + (hi + 1) * (int32_t)f->ch - 1 };
    t->all = false;
    return true;
}
