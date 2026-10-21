/*
 * drv/display/saturn.c -- the K1's HDMI DPU, on the mode U-Boot left
 *
 * U-Boot scans its framebuffer out on RDMA 3, in composer slot 0, the
 * top. display moves that slot to the bottom (7): it's plane 0, the
 * console's. Above it go an overlay, RDMA 2 in slot 6, and the cursor,
 * RDMA 0 in slot 0. RDMAs 2 and 3 share a line buffer, which U-Boot
 * leaves both at the start of, overlapping: the overlay gets the first
 * part, a line of the screen, and the console the rest (omega found
 * this on the board). The DPU's MMU stays off, as U-Boot left it.
 *
 * Vblank is the vsync interrupt, on PLIC line 139. A commit is latched
 * when CTL2_CFG_RDY reads 0 again, which it does at the start of the
 * frame, just before the vsync. saturn.h has the registers.
 */
#include "display.h"
#include "saturn.h"

static const unsigned rdma[3] = { S_CONSOLE_RDMA, 2, 0 };
static const unsigned slot[3] = { 7, 6, 0 };

static uint64_t spins(void)
{
    return 20000000;
}

static bool take(struct dp_out *o, bool)
{
    volatile uint8_t *r = o->regs;
    uint32_t act, line;
    unsigned s;

    if (o->regs_size < 0x1a000 || !(s_rd(r, S_CTL2_RCH_EN) & 1u << S_CONSOLE_RDMA))
        return false;               /* U-Boot didn't light it (no monitor) */
    for (unsigned n = 0; n < 4; n++)
        if (s_rd(r, S_CMDLIST_EN + 4 * n))
            return false;           /* the cmdlist would load over what we write */
    act = s_rd(r, S_OUT_ACTIVE);
    o->width = act & 0x3fff;
    o->height = act >> 16 & 0x3fff;
    if (!o->width || !o->height || (s = saturn_slot_of(r, S_CONSOLE_RDMA)) == S_SLOTS)
        return false;
    if (!saturn_quiet(r, spins()))
        return false;

    /* The console to the bottom; the line buffer split. Both latch
     * together, and the screen doesn't change. */
    line = (o->width * 4 + 63) / 64 * 2;
    if (s != slot[0]) {
        static const uint32_t regs[] = { S_SLOT_LEFT, S_SLOT_TOP_RIGHT, S_SLOT_BOTTOM,
                                         S_SLOT_ALPHA, S_SLOT_EN };

        for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
            s_wr(r, S_SLOT(slot[0]) + regs[i], s_rd(r, S_SLOT(s) + regs[i]));
        s_wr(r, S_SLOT(s) + S_SLOT_EN, 0);
    }
    s_wr(r, S_RDMA(2) + S_LINEBUF, 1u << 28 | line);
    s_wr(r, S_RDMA(S_CONSOLE_RDMA) + S_LINEBUF, line << 16 | (S_BANK1 - line));
    s_wr(r, S_RDMA(0) + S_LINEBUF, 1u << 28 | line);
    s_wr(r, S_CTL2_CFG_RDY, 1);
    for (uint64_t n = spins(); s_rd(r, S_CTL2_CFG_RDY) & 1;)
        if (n-- == 0)
            return false;

    o->nplanes = 3;
    for (unsigned p = 1; p < 3; p++)
        o->plane[p] = (struct DisplayPlaneInfo){
            .di_Formats = 1u << DISPLAY_XRGB8888 | 1u << DISPLAY_ARGB8888,
            .di_MaxWidth = o->width, .di_MaxHeight = o->height,
        };
    o->plane[2].di_Flags = DISPLAYP_CURSOR;
    o->con_fb = (volatile uint32_t *)o->fb;
    o->con_width = o->fb_cells[0];
    o->con_height = o->fb_cells[1];
    o->con_stride = o->fb_cells[2];
    o->con_bgr = o->fb_cells[3] == 1;
    o->plane[0] = (struct DisplayPlaneInfo){
        .di_Flags = DISPLAYP_CONSOLE, .di_Formats = 1u << DISPLAY_XRGB8888,
        .di_MaxWidth = o->con_width, .di_MaxHeight = o->con_height,
    };
    s_wr(r, S_INT_STATUS, S_INT_VSYNC);
    s_wr(r, S_INT_ENABLE, s_rd(r, S_INT_ENABLE) | S_INT_VSYNC);
    return true;
}

static void program(struct dp_out *o, const struct dp_hw *hw)
{
    volatile uint8_t *r = o->regs;
    uint32_t en = s_rd(r, S_CTL2_RCH_EN);

    for (unsigned p = 1; p < o->nplanes; p++) {
        uint32_t d = S_RDMA(rdma[p]), sl = S_SLOT(slot[p]);
        const struct dp_hw *h = &hw[p];

        if (!h->on) {
            s_wr(r, sl + S_SLOT_EN, 0);
            en &= ~(1u << rdma[p]);
            continue;
        }
        s_wr(r, d + S_LAYER_CTRL, S_LAYER_CTRL_RAW);
        s_wr(r, d + S_Y_OFST, h->y);
        s_wr(r, d + S_SCL_RATIO_V, 0);
        s_wr(r, d + S_ALPHA01, 0);
        s_wr(r, d + S_ALPHA23, 0);
        s_wr(r, d + S_BASE_LOW, (uint32_t)h->base);
        s_wr(r, d + S_BASE_HIGH, (uint32_t)(h->base >> 32) & 3);
        s_wr(r, d + S_STRIDE, h->stride);
        s_wr(r, d + S_IMG_SIZE, (h->sx + h->w) | (h->sy + h->h) << 16);
        s_wr(r, d + S_CROP_START, h->sx | h->sy << 16);
        s_wr(r, d + S_CROP_END, (h->sx + h->w - 1) | (h->sy + h->h - 1) << 16);
        s_wr(r, d + S_ROT_MODE, h->format == DISPLAY_ARGB8888 ? S_FMT_ARGB8888 : S_FMT_XRGB8888);
        s_wr(r, d + S_AFBC_CFG, 0);
        s_wr(r, S_TBU(rdma[p]), 0);
        s_wr(r, sl + S_SLOT_LEFT, h->x << 8);
        s_wr(r, sl + S_SLOT_TOP_RIGHT, h->y | (h->x + h->w - 1) << 16);
        s_wr(r, sl + S_SLOT_BOTTOM,
             (h->y + h->h - 1) | (h->format == DISPLAY_ARGB8888 ? S_PIXEL_ALPHA : 0));
        s_wr(r, sl + S_SLOT_ALPHA, 0xffu << 16);
        s_wr(r, sl + S_SLOT_EN, 1 | rdma[p] << 1);
        en |= 1u << rdma[p];
    }
    s_wr(r, S_CTL2_RCH_EN, en);
    s_wr(r, S_CTL2_CFG_RDY, 1);
}

static bool vblank(struct dp_out *o, bool *latched)
{
    volatile uint8_t *r = o->regs;
    uint32_t st = s_rd(r, S_INT_STATUS);

    s_wr(r, S_INT_STATUS, st);
    *latched = !(s_rd(r, S_CTL2_CFG_RDY) & 1);
    return (st & S_INT_VSYNC) != 0;
}

const struct dp_backend dp_saturn = {
    .name = "saturn", .take = take, .program = program, .vblank = vblank, .dma = true,
};
