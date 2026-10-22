/*
 * lib/gfx/bitmap.c -- AllocBitMap, FreeBitMap, GetBitMapAttr, InitBitMap
 * and InitRastPort (V7 step 2, docs §16.3 pillar 1)
 *
 * An allocated bitmap is one Area of the caller's: the pixels from its
 * start, and the BitMap itself after the last row, so nothing else holds
 * it. With BMF_DISPLAYABLE and a friend, the Area is placed as the
 * friend's is (AREAF_NC, AREAF_DMA32: QueryArea's flags), and a bitmap
 * as wide as the friend takes its rows' stride.
 */
#include "rows.h"

static bool format_ok(uint32_t format)
{
    return format == PIXFMT_XRGB8888 || format == PIXFMT_ARGB8888;
}

struct BitMap *AllocBitMap(uint32_t width, uint32_t height, uint32_t format, uint32_t flags,
                           const struct BitMap *friend_bm)
{
    uint64_t bpr = ((uint64_t)width * 4 + GFX_ROW_ALIGN - 1) / GFX_ROW_ALIGN * GFX_ROW_ALIGN;
    uint32_t aflags = 0;
    struct AreaInfo ai;
    struct BitMap *bm;
    void *base;
    int64_t h;

    if (!width || !height || width > GFX_MAX_SIZE || height > GFX_MAX_SIZE || !format_ok(format) ||
        (flags & ~(BMF_CLEAR | BMF_DISPLAYABLE)))
        return nullptr;
    if ((flags & BMF_DISPLAYABLE) && friend_bm) {
        if (friend_bm->bm_Width == width && friend_bm->bm_BytesPerRow >= bpr)
            bpr = friend_bm->bm_BytesPerRow;
        if (friend_bm->bm_BaseAddr && QueryArea(friend_bm->bm_BaseAddr, &ai) == 0)
            aflags = ai.ai_Flags & (AREAF_NC | AREAF_DMA32);
    }
    if ((h = AllocArea(bpr * height + sizeof *bm, aflags, &base)) < 0)
        return nullptr;
    bm = (struct BitMap *)((uint8_t *)base + bpr * height);
    *bm = (struct BitMap){
        .bm_Area = (Handle)h,
        .bm_BaseAddr = base,
        .bm_Width = width,
        .bm_Height = height,
        .bm_BytesPerRow = (uint32_t)bpr,
        .bm_PixelFormat = format,
        .bm_Modifier = MOD_LINEAR,
        .bm_Flags = flags | BMF_CLEAR,      /* an Area always is */
        .bm_ColorSpace = CS_SRGB,
    };
    return bm;
}

/* The BitMap is in the Area it frees: its handle is read first. */
void FreeBitMap(struct BitMap *bm)
{
    Handle h = bm ? bm->bm_Area : 0;

    if (h)
        FreeArea(h);
}

uint64_t GetBitMapAttr(const struct BitMap *bm, uint32_t attr)
{
    if (!bm)
        return 0;
    switch (attr) {
    case BMA_HEIGHT: return bm->bm_Height;
    case BMA_DEPTH: return 32;
    case BMA_WIDTH: return bm->bm_Width;
    case BMA_FLAGS: return bm->bm_Flags;
    case BMA_PIXELFORMAT: return bm->bm_PixelFormat;
    case BMA_BYTESPERROW: return bm->bm_BytesPerRow;
    case BMA_MODIFIER: return bm->bm_Modifier;
    case BMA_BASEADDR: return (uint64_t)(uintptr_t)bm->bm_BaseAddr;
    case BMA_COLORSPACE: return bm->bm_ColorSpace;
    case BMA_AREA: return bm->bm_Area;
    default: return 0;
    }
}

bool InitBitMap(struct BitMap *bm, void *base, uint32_t width, uint32_t height,
                uint32_t bytesPerRow, uint32_t format)
{
    if (!bm || !base || ((uintptr_t)base & 3) || !width || !height || width > GFX_MAX_SIZE ||
        height > GFX_MAX_SIZE || (bytesPerRow & 3) || bytesPerRow < (uint64_t)width * 4 ||
        !format_ok(format))
        return false;
    *bm = (struct BitMap){
        .bm_BaseAddr = base,
        .bm_Width = width,
        .bm_Height = height,
        .bm_BytesPerRow = bytesPerRow,
        .bm_PixelFormat = format,
        .bm_Modifier = MOD_LINEAR,
        .bm_ColorSpace = CS_SRGB,
    };
    return true;
}

/* The whole bitmap, white on black, JAM2, the 8×8 font. */
void InitRastPort(struct RastPort *rp, struct BitMap *bm)
{
    if (!rp)
        return;
    *rp = (struct RastPort){
        .rp_BitMap = bm,
        .rp_Clip = { 0, 0, bm ? (int32_t)bm->bm_Width - 1 : -1, bm ? (int32_t)bm->bm_Height - 1 : -1 },
        .rp_FgPen = 0xffffffffu,
        .rp_BgPen = 0xff000000u,
        .rp_DrawMode = JAM2,
    };
}
