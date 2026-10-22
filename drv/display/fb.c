/*
 * drv/display/fb.c -- the firmware's framebuffer, with no controller
 *
 * QEMU's (edk2's GOP on ramfb, or a device tree's simple-framebuffer),
 * and any machine's whose controller display doesn't drive. Nothing
 * composes planes there but this file: the console draws into a copy of
 * the screen in memory, and what the screen shows is that with the
 * planes over it, written out where something changed. A frame is a
 * tick of 60 Hz, on the driver's deadline: a commit is composed at the
 * next, its "latch".
 *
 * The copy starts as what the screen shows, so a restarted display goes
 * on from the last one's picture. The planes' buffers are DMA Areas, as
 * a controller's are, though only display reads them: their life across
 * display's death is the same on QEMU as on a board, held until hwdb
 * has stopped what reads them (server.c's stop_display, a no-op here).
 */
#include "display.h"

constexpr uint64_t FRAME_NS = 16666667;

static struct dp_hw shown[DISPLAY_PLANES], next[DISPLAY_PLANES];
static bool pending;
static uint64_t tick;
static Handle copy_area;

static uint32_t swap_rb(uint32_t p)
{
    return (p & 0xff00ff00) | (p & 0xff) << 16 | (p >> 16 & 0xff);
}

static uint32_t blend(uint32_t src, uint32_t dst)
{
    uint32_t a = src >> 24, out = 0;

    if (a == 0xff)
        return src;
    for (unsigned sh = 0; sh < 24; sh += 8) {
        uint32_t s = src >> sh & 0xff, d = dst >> sh & 0xff;

        out |= (s * a + d * (255 - a) + 127) / 255 << sh;
    }
    return out;
}

/* Rows y0 to y1 of the screen: the console's, then each plane over it. */
static void compose(struct dp_out *o, uint32_t y0, uint32_t y1)
{
    volatile uint32_t *screen = (volatile uint32_t *)o->fb;
    uint32_t stride = o->fb_cells[2];

    if (y1 > o->height)
        y1 = o->height;
    for (uint32_t y = y0; y < y1; y++) {
        volatile uint32_t *row = screen + (uint64_t)y * stride;
        const volatile uint32_t *con = o->con_fb + (uint64_t)y * o->con_stride;

        for (uint32_t x = 0; x < o->width; x++)
            row[x] = con[x];
        for (unsigned p = 1; p < o->nplanes; p++) {
            const struct dp_hw *h = &shown[p];
            const volatile uint32_t *src;

            if (!h->on || y < h->y || y >= h->y + h->h)
                continue;
            src = (const volatile uint32_t *)(h->base + (uint64_t)(h->sy + y - h->y) * h->stride) +
                  h->sx;
            for (uint32_t x = 0; x < h->w; x++) {
                uint32_t px = h->format == DISPLAY_XRGB8888 ? src[x] | 0xff000000 : src[x];

                if (!o->con_bgr)
                    px = swap_rb(px);
                row[h->x + x] = blend(px, row[h->x + x]) & 0xffffff;
            }
        }
    }
    if (y1 > y0)
        dp_clean((const volatile void *)(screen + (uint64_t)y0 * stride),
                 (uint64_t)(y1 - y0) * stride * 4);
}

static bool take(struct dp_out *o, bool)
{
    uint64_t size, addr;
    const volatile uint32_t *screen = (const volatile uint32_t *)o->fb;
    volatile uint32_t *c;
    int64_t e;

    o->width = o->fb_cells[0];
    o->height = o->fb_cells[1];
    if (!o->width || !o->height || o->fb_cells[2] < o->width || o->fb_cells[3] > 1 ||
        (uint64_t)o->fb_cells[2] * o->height * 4 > o->fb_size)
        return false;
    size = ((uint64_t)o->width * o->height * 4 + 4095) & ~4095ull;
    if ((e = dp_alloc(o, size, false, false, &copy_area, &addr)) != 0)
        return false;
    c = (volatile uint32_t *)addr;
    for (uint32_t y = 0; y < o->height; y++)
        for (uint32_t x = 0; x < o->width; x++)
            c[(uint64_t)y * o->width + x] = screen[(uint64_t)y * o->fb_cells[2] + x];
    o->con_fb = c;
    o->con_width = o->width;
    o->con_height = o->height;
    o->con_stride = o->width;
    o->con_bgr = o->fb_cells[3] == 1;
    o->nplanes = 3;
    o->plane[0] = (struct DisplayPlaneInfo){
        .di_Flags = DISPLAYP_CONSOLE, .di_Formats = 1u << DISPLAY_XRGB8888,
        .di_MaxWidth = o->width, .di_MaxHeight = o->height,
    };
    for (unsigned p = 1; p < 3; p++)
        o->plane[p] = (struct DisplayPlaneInfo){
            .di_Formats = 1u << DISPLAY_XRGB8888 | 1u << DISPLAY_ARGB8888,
            .di_MaxWidth = o->width, .di_MaxHeight = o->height,
        };
    o->plane[2].di_Flags = DISPLAYP_CURSOR;
    o->refresh_mhz = 60000;
    for (unsigned p = 0; p < DISPLAY_PLANES; p++)
        shown[p] = next[p] = (struct dp_hw){ 0 };
    pending = false;
    tick = dp_now() + FRAME_NS;
    return true;
}

static void program(struct dp_out *o, const struct dp_hw *hw)
{
    for (unsigned p = 1; p < o->nplanes; p++)
        next[p] = hw[p];
    pending = true;
}

/* The rows a plane covers, old or new, composed again. */
static void recompose(struct dp_out *o, const struct dp_hw *a, const struct dp_hw *b)
{
    for (unsigned p = 1; p < o->nplanes; p++) {
        if (a[p].on)
            compose(o, a[p].y, a[p].y + a[p].h);
        if (b[p].on)
            compose(o, b[p].y, b[p].y + b[p].h);
    }
}

static bool vblank(struct dp_out *o, bool *latched)
{
    uint64_t now = dp_now();
    struct dp_hw was[DISPLAY_PLANES];

    *latched = false;
    if (now < tick)
        return false;
    while (tick <= now)
        tick += FRAME_NS;
    if (pending) {
        for (unsigned p = 0; p < DISPLAY_PLANES; p++) {
            was[p] = shown[p];
            shown[p] = next[p];
        }
        recompose(o, was, shown);
        pending = false;
        *latched = true;
    }
    return true;
}

static void damage(struct dp_out *o, uint32_t y0, uint32_t y1)
{
    compose(o, y0, y1);
}

static uint64_t deadline(struct dp_out *)
{
    return tick;
}

static uint32_t pixel(const struct dp_out *o, uint32_t x, uint32_t y)
{
    uint32_t px = ((const volatile uint32_t *)o->fb)[(uint64_t)y * o->fb_cells[2] + x];

    return (o->con_bgr ? px : swap_rb(px)) & 0xffffff;
}

const struct dp_backend dp_fb = {
    .name = "fb", .take = take, .program = program, .vblank = vblank, .damage = damage,
    .deadline = deadline, .pixel = pixel, .dma = true,
};
