/*
 * drv/display/komeda.c -- the CIX's DPU4, on the mode the GOP left
 *
 * The GOP scans its framebuffer out through pipeline 0: its layers, and
 * on this board its scalers and merger, down to the mode it chose (no
 * EDID comes through the PS185, and it falls back to 640x480). That's
 * plane 0, the console's, and display never changes it. The planes
 * above it are two layers the GOP left off, each fed straight into a
 * compiz input above the GOP's: an overlay, and the cursor on top. They
 * are at the output's size, not the framebuffer's.
 *
 * Vblank is the DOU's PL0 interrupt. A commit is latched when
 * GCU_CONFIG_VALID0 reads 0 again. The GOP leaves PL0 at line 0, before
 * the prefetch line where the DPU takes a commit: a commit made at the
 * vblank then missed that frame and every flip was a frame late. PL0 is
 * moved to the line after it, as the vendor's driver has it. komeda.h
 * has the registers.
 */
#include "display.h"
#include "komeda.h"

/* be_words: */
enum { W_CU, W_DOU, W_LPU, W_LAYER, W_INPUT = W_LAYER + DISPLAY_PLANES };

static uint64_t spins(void)
{
    return 20000000;                /* a frame at 640x480 is 17 ms; far more than that */
}

/* What the GOP set up, said once: its layers, the compiz's inputs. */
static void report(volatile uint8_t *r, uint32_t cu)
{
    char line[320];                 /* KLog keeps 200 */
    unsigned n = 0;

    for (const char *t = "komeda: GOP layers"; *t; t++)
        line[n++] = *t;
    for (unsigned id = 0; id < 4; id++) {
        uint32_t l = komeda_find(r, K_LAYER, id);

        if (!l || !(k_rd(r, l + K_CONTROL) & K_L_EN))
            continue;
        line[n++] = ' ';
        n += dp_dec(line + n, id);
        line[n++] = '@';
        n += dp_hex(line + n, (uint64_t)k_rd(r, l + K_P0_PTR_HIGH) << 32 | k_rd(r, l + K_P0_PTR_LOW));
        line[n++] = '/';
        n += dp_dec(line + n, k_rd(r, l + K_IN_SIZE) & 0x1fff);
        line[n++] = 'x';
        n += dp_dec(line + n, k_rd(r, l + K_IN_SIZE) >> 16 & 0x1fff);
        line[n++] = '/';
        n += dp_hex(line + n, k_rd(r, l + K_LAYER_FMT));
    }
    for (const char *t = "; inputs"; *t; t++)
        line[n++] = *t;
    for (unsigned i = 0; i < KOMEDA_INPUTS; i++) {
        line[n++] = ' ';
        n += dp_hex(line + n, k_rd(r, cu + K_CU_INPUT_ID(i)));
    }
    line[n] = '\0';
    dp_say(line);
}

static bool take(struct dp_out *o, bool again)
{
    volatile uint8_t *r = o->regs;
    uint64_t *w = o->be_words;
    uint32_t cu, dou, bs, size, top = 0;
    unsigned free = 0;

    if (o->regs_size < 0x2000 || (k_rd(r, K_GLB_CORE_ID) >> 16) != 0x0060 ||
        (k_rd(r, K_CONTROL) & K_GCU_MODE) == 0)
        return false;               /* not a D60, or not lit */
    cu = komeda_find(r, K_CU, 0);
    dou = komeda_find(r, K_DOU, 0);
    bs = komeda_find(r, K_BS, 0);
    if (!cu || !dou || !bs || !(k_rd(r, bs + K_CONTROL) & 1))
        return false;
    size = k_rd(r, bs + K_BS_ACTIVESIZE);
    o->width = size & 0x1fff;
    o->height = size >> 16 & 0x1fff;
    if (!o->width || !o->height)
        return false;
    /* A first start finds the GOP's alone, and leaves every one. A dead
     * display's planes are off already (hwdb's stop), unless it died
     * before hwdb could: off now. */
    if (!again)
        report(r, cu);
    else if (!komeda_quiet(r, (uint64_t)o->fb, (uint64_t)o->fb + o->fb_size, spins()))
        return false;

    w[W_CU] = cu;
    w[W_DOU] = dou;
    w[W_LPU] = komeda_find(r, K_LPU, 0);
    for (unsigned n = 0; n < KOMEDA_INPUTS; n++)
        if (k_rd(r, cu + K_CU_INPUT_ID(n)))
            top = n + 1;            /* the GOP's inputs: ours go above */
    o->nplanes = 1;
    for (unsigned id = 0; id < 4 && o->nplanes < 3 && top + free < KOMEDA_INPUTS; id++) {
        uint32_t l = komeda_find(r, K_LAYER, id);

        if (!l || (k_rd(r, l + K_CONTROL) & K_L_EN))
            continue;
        w[W_LAYER + o->nplanes] = l;
        w[W_INPUT + o->nplanes] = top + free++;
        o->plane[o->nplanes] = (struct DisplayPlaneInfo){
            .di_Formats = 1u << DISPLAY_XRGB8888 | 1u << DISPLAY_ARGB8888,
            .di_MaxWidth = o->width, .di_MaxHeight = o->height,
        };
        o->nplanes++;
    }
    if (o->nplanes == 3)
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
    /* PL0 a line after the fetch starts, as the vendor's driver has it:
     * the commit for the next frame is taken then, and a client woken
     * at the vblank has a frame to make it. */
    {
        uint32_t prog = k_rd(r, bs + K_BS_PROG_LINE), pre = k_rd(r, bs + K_BS_PREFETCH_LINE);
        char line[120];
        unsigned n = 0;

        for (const char *t = "komeda: PL0 at line "; *t; t++)
            line[n++] = *t;
        n += dp_dec(line + n, prog);
        for (const char *t = ", prefetch at "; *t; t++)
            line[n++] = *t;
        n += dp_dec(line + n, pre);
        for (const char *t = ", vertical porches "; *t; t++)
            line[n++] = *t;
        n += dp_hex(line + n, k_rd(r, bs + K_BS_VINTERVALS));
        line[n] = '\0';
        if (!again)
            dp_say(line);
        if (pre >= 1 && prog != pre - 1)
            k_wr(r, bs + K_BS_PROG_LINE, pre - 1);
    }
    k_wr(r, dou + K_IRQ_MASK, k_rd(r, dou + K_IRQ_MASK) | K_DOU_PL0);
    return true;
}

static void program(struct dp_out *o, const struct dp_hw *hw)
{
    volatile uint8_t *r = o->regs;
    const uint64_t *w = o->be_words;
    uint32_t cu = (uint32_t)w[W_CU];

    for (unsigned p = 1; p < o->nplanes; p++) {
        uint32_t l = (uint32_t)w[W_LAYER + p];
        unsigned n = (unsigned)w[W_INPUT + p];
        uint64_t at = hw[p].base + (uint64_t)hw[p].sy * hw[p].stride + (uint64_t)hw[p].sx * 4;

        if (!hw[p].on) {
            k_wr(r, l + K_CONTROL, K_ARCACHE);
            k_wr(r, cu + K_CU_INPUT_ID(n), 0);
            k_wr(r, cu + K_CU_CTRL(n), 0);
            continue;
        }
        k_wr(r, l + K_P0_STRIDE, hw[p].stride);
        k_wr(r, l + K_P0_PTR_LOW, (uint32_t)at);
        k_wr(r, l + K_P0_PTR_HIGH, (uint32_t)(at >> 32));
        k_wr(r, l + K_LAYER_FMT,
             hw[p].format == DISPLAY_ARGB8888 ? K_FMT_ARGB8888 : K_FMT_XRGB8888);
        k_wr(r, l + K_IN_SIZE, hw[p].w | hw[p].h << 16);
        k_wr(r, l + K_AD_CONTROL, 0);
        k_wr(r, l + K_CONTROL, K_ARCACHE | K_L_EN);
        k_wr(r, cu + K_CU_INPUT_ID(n), k_rd(r, l + K_BLOCK_INFO) & 0xfff0);
        k_wr(r, cu + K_CU_SIZE(n), hw[p].w | hw[p].h << 16);
        k_wr(r, cu + K_CU_OFFSET(n), hw[p].x | hw[p].y << 16);
        k_wr(r, cu + K_CU_CTRL(n),
             K_CU_EN | (hw[p].format == DISPLAY_XRGB8888 ? K_CU_PAD : 0) | 0xffu << 8);
    }
    k_wr(r, K_GCU_CONFIG_VALID0, 1);
}

static bool vblank(struct dp_out *o, bool *latched)
{
    volatile uint8_t *r = o->regs;
    const uint64_t *w = o->be_words;
    uint32_t status = k_rd(r, K_GLB_IRQ_STATUS);
    const struct { uint32_t bit, blk; } src[] = {
        { 1u << 0, 0 }, { 1u << 8, (uint32_t)w[W_LPU] }, { 1u << 16, (uint32_t)w[W_CU] },
        { 1u << 24, (uint32_t)w[W_DOU] },
    };
    bool vb = false;

    /* Each block that's asking, cleared: the line is level-triggered. */
    for (unsigned i = 0; i < sizeof src / sizeof src[0]; i++) {
        uint32_t raw;

        if (!(status & src[i].bit) || (i && !src[i].blk))
            continue;
        raw = k_rd(r, src[i].blk + K_IRQ_RAW);
        k_wr(r, src[i].blk + K_IRQ_CLEAR, raw);
        if (src[i].blk == w[W_DOU])
            vb |= (raw & K_DOU_PL0) != 0;
    }
    *latched = !(k_rd(r, K_GCU_CONFIG_VALID0) & 1);
    return vb;
}

const struct dp_backend dp_komeda = {
    .name = "komeda", .take = take, .program = program, .vblank = vblank, .dma = true,
};
