/*
 * lib/gfx/text.c -- Text in the 8×8 font, or a rastport's own (V7 step 2)
 *
 * A glyph at a time, a pixel at a time, clipped: 8 pixels a row leave a
 * vector unit nothing to do, so this is the one operation with no vector
 * entry. A character the font doesn't hold is drawn as its first glyph
 * (the 8×8's space).
 */
#include <planar/gfx.h>

#include "font8x8.h"

static const struct TextFont eight = {
    .tf_YSize = 8,
    .tf_XSize = 8,
    .tf_LoChar = FONT8X8_FIRST,
    .tf_HiChar = FONT8X8_FIRST + FONT8X8_COUNT - 1,
    .tf_CharData = font8x8[0],
};

void Text(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count)
{
    const struct TextFont *f;
    const struct BitMap *bm;
    int64_t cx0, cy0, cx1, cy1, gx = x;

    if (!rp || !(bm = rp->rp_BitMap) || !bm->bm_BaseAddr || !string)
        return;
    f = rp->rp_Font ? rp->rp_Font : &eight;
    if (!f->tf_CharData || f->tf_XSize > 8 || f->tf_LoChar > f->tf_HiChar)
        return;
    cx0 = rp->rp_Clip.MinX > 0 ? rp->rp_Clip.MinX : 0;
    cy0 = rp->rp_Clip.MinY > 0 ? rp->rp_Clip.MinY : 0;
    cx1 = rp->rp_Clip.MaxX < (int64_t)bm->bm_Width - 1 ? rp->rp_Clip.MaxX : (int64_t)bm->bm_Width - 1;
    cy1 = rp->rp_Clip.MaxY < (int64_t)bm->bm_Height - 1 ? rp->rp_Clip.MaxY : (int64_t)bm->bm_Height - 1;
    for (uint64_t i = 0; i < count && gx <= cx1; i++, gx += f->tf_XSize) {
        unsigned c = (unsigned char)string[i];
        const uint8_t *g = f->tf_CharData +
            (uint64_t)(c >= f->tf_LoChar && c <= f->tf_HiChar ? c - f->tf_LoChar : 0) * f->tf_YSize;

        if (gx + f->tf_XSize - 1 < cx0)
            continue;
        for (int64_t r = 0; r < f->tf_YSize; r++) {
            int64_t py = y + r;
            uint32_t *row;

            if (py < cy0 || py > cy1)
                continue;
            row = (uint32_t *)((uint8_t *)bm->bm_BaseAddr + (uint64_t)py * bm->bm_BytesPerRow);
            for (int64_t b = 0; b < f->tf_XSize; b++) {
                int64_t px = gx + b;

                if (px < cx0 || px > cx1)
                    continue;
                if (g[r] >> b & 1)
                    row[px] = rp->rp_FgPen;
                else if (rp->rp_DrawMode == JAM2)
                    row[px] = rp->rp_BgPen;
            }
        }
    }
}
