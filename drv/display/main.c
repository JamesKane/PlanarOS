/*
 * drv/display/main.c -- a text console on the firmware's framebuffer
 * (v4-plan step 7; decision 2: the firmware's, native scanout in V5)
 *
 * The framebuffer the firmware left set up (UEFI's GOP, or on the K1 U-Boot's
 * display layer), as hwdb describes it (its geometry in the node's
 * cells), leased like registers. What's drawn is cleaned from the caches
 * after each write, for a controller that doesn't snoop them (the K1's). Text in
 * the 8x8 font, twice as big on screens 1600 pixels wide or more,
 * scrolling up a line at a time; the rest of the screen is the
 * background.
 *
 * It serves "con.N": a DRV_WRITE of up to 64 bytes of text at a time,
 * which also goes, a line at a time, to the kernel's log, and so to the
 * serial console (decision 1: the console is both).
 */
#include <planar/drv.h>
#include <planar/syscalls.h>

#include "font8x8.h"

constexpr uint32_t FG = 0xe8e8e8;           /* light grey on dark slate blue, omega's */
constexpr uint32_t BG = 0x203050;
constexpr unsigned LINE = 160;

struct con {
    volatile uint32_t *fb;
    uint32_t width, height, stride;
    bool bgr;                               /* GOP format 1: blue in the low byte */
    uint32_t scale, cell, cols, rows, col, row;
    char line[LINE];                        /* for the log */
    unsigned nline;
    uint32_t dirty_lo, dirty_hi;            /* rows changed since the last clean */
};

static struct con cons[DRV_DEVICES];

/* A colour as 0xRRGGBB, as this framebuffer holds it. */
static uint32_t pixel(const struct con *c, uint32_t rgb)
{
    return c->bgr ? rgb : (rgb & 0xff) << 16 | (rgb & 0xff00) | rgb >> 16;
}

static void dirty(struct con *c, uint32_t y0, uint32_t y1)
{
    if (y0 < c->dirty_lo)
        c->dirty_lo = y0;
    if (y1 > c->dirty_hi)
        c->dirty_hi = y1 < c->height ? y1 : c->height;
}

/* The rows drawn, out of the caches to memory: a display controller that
 * doesn't snoop (the K1's) scans memory. */
static void clean(struct con *c)
{
    if (c->dirty_hi > c->dirty_lo)
        drv_dma_clean((const void *)(c->fb + (uint64_t)c->dirty_lo * c->stride),
                      (uint64_t)(c->dirty_hi - c->dirty_lo) * c->stride * 4);
    c->dirty_lo = UINT32_MAX;
    c->dirty_hi = 0;
}

static void fill(struct con *c, uint32_t y0, uint32_t y1, uint32_t rgb)
{
    uint32_t p = pixel(c, rgb);

    dirty(c, y0, y1);
    for (uint32_t y = y0; y < y1 && y < c->height; y++)
        for (uint32_t x = 0; x < c->width; x++)
            c->fb[(uint64_t)y * c->stride + x] = p;
}

static void glyph(struct con *c, char ch)
{
    const uint8_t *g = font8x8[0];
    uint32_t fg = pixel(c, FG), bg = pixel(c, BG);
    uint32_t x0 = c->col * c->cell, y0 = c->row * c->cell;

    if ((unsigned char)ch >= FONT8X8_FIRST && (unsigned char)ch < FONT8X8_FIRST + FONT8X8_COUNT)
        g = font8x8[(unsigned char)ch - FONT8X8_FIRST];
    dirty(c, y0, y0 + c->cell);
    for (uint32_t y = 0; y < c->cell; y++)
        for (uint32_t x = 0; x < c->cell; x++)
            c->fb[(uint64_t)(y0 + y) * c->stride + x0 + x] =
                g[y / c->scale] >> (x / c->scale) & 1 ? fg : bg;
}

static void scroll(struct con *c)
{
    uint32_t keep = (c->rows - 1) * c->cell;

    dirty(c, 0, c->rows * c->cell);
    for (uint32_t y = 0; y < keep; y++)
        for (uint32_t x = 0; x < c->cols * c->cell; x++)
            c->fb[(uint64_t)y * c->stride + x] = c->fb[(uint64_t)(y + c->cell) * c->stride + x];
    fill(c, keep, c->rows * c->cell, BG);
}

static void newline(struct con *c)
{
    c->col = 0;
    if (++c->row == c->rows) {
        scroll(c);
        c->row = c->rows - 1;
    }
}

static void log_line(struct con *c)
{
    c->line[c->nline] = '\0';
    KLog(c->line);
    c->nline = 0;
}

static void put(struct con *c, char ch)
{
    if (ch == '\n') {
        newline(c);
        log_line(c);
        return;
    }
    if (ch == '\r')
        return;
    if (ch == '\b') {
        if (c->col)
            c->col--;
        if (c->nline)
            c->nline--;
        glyph(c, ' ');
        return;
    }
    if (ch == '\t')
        ch = ' ';
    glyph(c, ch);
    if (++c->col == c->cols)
        newline(c);
    if (c->nline < LINE - 1)
        c->line[c->nline++] = ch;
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    struct con *c = &cons[dev - d->dev];
    const struct DrvDevice *info = dev->info;

    *c = (struct con){
        .fb = (volatile uint32_t *)dev->regs, .width = info->dd_Cells[0],
        .height = info->dd_Cells[1], .stride = info->dd_Cells[2], .bgr = info->dd_Cells[3] == 1,
    };
    if (!c->width || !c->height || c->stride < c->width || info->dd_Cells[3] > 1 ||
        (uint64_t)c->stride * c->height * 4 > info->dd_Size)
        return DRV_DECLINE;
    c->scale = c->width >= 1600 ? 2 : 1;
    c->cell = 8 * c->scale;
    c->cols = c->width / c->cell;
    c->rows = c->height / c->cell;
    c->dirty_lo = UINT32_MAX;
    if (c->cols < 20 || c->rows < 4)
        return DRV_DECLINE;
    fill(c, 0, c->height, BG);
    clean(c);
    return DRV_OK;
}

static void num(char *line, unsigned *n, uint32_t v)
{
    char dg[12];
    unsigned k = 0;

    do
        dg[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < sizeof dg);
    while (k)
        line[(*n)++] = dg[--k];
}

static int start(struct drv *d, struct drv_dev *dev)
{
    struct con *c = &cons[dev - d->dev];
    char line[80];
    unsigned n = 0;

    for (const char *s = "Planar"; *s; s++, c->col++)
        glyph(c, *s);
    newline(c);
    newline(c);
    num(line, &n, c->width);
    line[n++] = 'x';
    num(line, &n, c->height);
    for (const char *s = ", a text console on it"; *s; s++)
        line[n++] = *s;
    line[n] = '\0';
    clean(c);
    drv_log(d, dev, line);
    return (int)drv_publish(d, dev, "con");
}

static void request(struct drv *d, struct drv_dev *dev, struct DrvRequest *r)
{
    struct con *c = &cons[dev - d->dev];

    if (r->dr_Op != DRV_WRITE) {
        r->dr_Msg.mn_Error = (int32_t)ERR_BADARG;
        return;
    }
    for (uint32_t i = 0; i < r->dr_Len; i++)
        put(c, (char)r->dr_Data[i]);
    clean(c);
    r->dr_Msg.mn_Error = 0;
}

static const struct drv_ops ops = {
    .name = "display", .bind = bind, .start = start, .request = request,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
