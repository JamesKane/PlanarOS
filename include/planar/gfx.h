/*
 * planar/gfx.h -- gfx.library: bitmaps, rastports and drawing on the CPU
 *
 * V7 step 2 (docs §16.3 pillar 1, §16.4); V8 step 3's paths, gradients
 * and type, font.library's glyphs drawn and cached. A BitMap describes 32-bit
 * pixels in memory: an Area of the caller's (AllocBitMap), or memory it
 * has from elsewhere, a display buffer, say (InitBitMap). A RastPort is
 * where to draw in one: the bitmap, a clip rectangle, the pens, the draw
 * mode and the font. Both are the caller's: the library keeps nothing.
 *
 * Every operation clips to the rastport's clip rectangle, which is
 * itself clipped to its bitmap, and draws on the CPU: scalar, or by the
 * vector unit (RVV 1.0, NEON) where the CPU has one, the kernel pointing
 * the table's entries at those at boot. The two give the same pixels.
 *
 * Pixels are 32 bits, blue in the low byte (0xAARRGGBB as a uint32_t),
 * as display's formats are. Pens are pixels. Coordinates are signed: a
 * rectangle may start off the bitmap, and is clipped.
 */
#ifndef PLANAR_GFX_H
#define PLANAR_GFX_H

#include <stdint.h>

#include <planar/exec.h>

/* BitMap.bm_PixelFormat: display's DISPLAY_XRGB8888 and DISPLAY_ARGB8888. */
enum : uint32_t {
    PIXFMT_XRGB8888 = 1,        /* opaque: the top byte isn't read */
    PIXFMT_ARGB8888 = 2,        /* alpha in the top byte, not premultiplied */
};

constexpr uint64_t MOD_LINEAR = 0;          /* rows one after another: the only layout */
constexpr uint32_t CS_SRGB = 0;             /* the only colour space */

/* AllocBitMap's flags, kept in bm_Flags. */
constexpr uint32_t BMF_CLEAR = 1u << 0;     /* zeroed (an Area always is) */
constexpr uint32_t BMF_DISPLAYABLE = 1u << 1;   /* placed as its friend is */

constexpr uint32_t GFX_ROW_ALIGN = 64;      /* bm_BytesPerRow's multiple, from AllocBitMap */
constexpr uint32_t GFX_MAX_SIZE = 16384;    /* a bitmap's width or height, at most */

struct BitMap {
    Handle bm_Area;             /* 0: the caller's memory (InitBitMap) */
    uint32_t bm_Pad;
    void *bm_BaseAddr;          /* the top left pixel */
    uint32_t bm_Width, bm_Height;
    uint32_t bm_BytesPerRow;
    uint32_t bm_PixelFormat;    /* PIXFMT_* */
    uint64_t bm_Modifier;       /* MOD_LINEAR */
    uint32_t bm_Flags;          /* BMF_* */
    uint32_t bm_ColorSpace;     /* CS_SRGB */
};

/* GetBitMapAttr's attributes (V39's BMA_*, and Planar's). */
enum : uint32_t {
    BMA_HEIGHT = 0,
    BMA_DEPTH = 4,              /* bits a pixel: 32 */
    BMA_WIDTH = 8,
    BMA_FLAGS = 12,
    BMA_PIXELFORMAT = 16,
    BMA_BYTESPERROW = 20,
    BMA_MODIFIER = 24,
    BMA_BASEADDR = 28,
    BMA_COLORSPACE = 32,
    BMA_AREA = 36,
};

/*
 * A font: glyphs at most 8 pixels wide, a byte a row, bit 0 leftmost
 * (the 8×8, or the caller's); or, from OpenTextFont (V8 step 3), one of
 * font.library's at a size, its outlines drawn as they're needed and
 * kept in a cache of its own. An outline font has no tf_CharData.
 */
struct GfxFontCache;

struct TextFont {
    uint16_t tf_YSize, tf_XSize;            /* the line's height; a glyph's width, 0 if they vary */
    uint16_t tf_LoChar, tf_HiChar;          /* the glyphs held; others are tf_LoChar's */
    const uint8_t *tf_CharData;             /* tf_YSize bytes a glyph, from tf_LoChar */
    /* V8 step 3: an outline font's. */
    uint16_t tf_Baseline;                   /* the line's top to its baseline, pixels */
    uint16_t tf_Flags;                      /* FPF_* */
    uint32_t tf_Size;                       /* 26.6 px an em */
    struct GfxFontCache *tf_Cache;          /* OpenTextFont's; nullptr for a bitmap font */
};

/* TextFont.tf_Flags */
constexpr uint16_t FPF_OUTLINE = 1u << 0;   /* font.library's, from OpenTextFont */
constexpr uint16_t FPF_FIXED = 1u << 1;     /* every glyph tf_XSize wide */

/* What OpenTextFont asks for, as the RKM's TextAttr: "System Sans" (or
 * nullptr) or "System Mono", pixels an em, and FSF_BOLD or not. */
struct TextAttr {
    const char *ta_Name;
    uint16_t ta_YSize;                      /* 1 to 255 */
    uint8_t ta_Style;                       /* FSF_* */
    uint8_t ta_Flags;                       /* 0 */
};

constexpr uint8_t FSF_BOLD = 1u << 1;       /* the RKM's bit */

/* Scratch for FillPath and FillGradient's paths, the caller's, as the
 * RKM's TmpRas is AreaEnd's: without one they draw in small pieces, on
 * the stack; with one, in pieces as big as it holds. */
struct TmpRas {
    void *tr_RasPtr;
    uint64_t tr_Size;
};

/* RastPort.rp_DrawMode */
enum : uint32_t {
    JAM1 = 0,                   /* Text: the foreground pen alone */
    JAM2 = 1,                   /* Text: the background pen behind it */
};

/* GfxRect: inclusive, as RectFill's are. */
struct GfxRect {
    int32_t MinX, MinY, MaxX, MaxY;
};

struct RastPort {
    struct BitMap *rp_BitMap;
    struct GfxRect rp_Clip;     /* where drawing may land, clipped again to the bitmap */
    uint32_t rp_FgPen, rp_BgPen;
    uint32_t rp_DrawMode;       /* JAM1, JAM2 */
    uint32_t rp_Pad;
    const struct TextFont *rp_Font;     /* nullptr: the 8×8 font */
    struct TmpRas *rp_TmpRas;           /* nullptr: none */
};

/*
 * A path (V8 step 1): outlines of lines and Bézier curves, in 26.6 fixed
 * point (1/64 px), y down. Its arrays are the caller's: gp_Max* their
 * sizes, gp_Num* what's in them. Each verb takes its points from the
 * next in gp_Points: MOVE and LINE one, QUAD two (the control, then the
 * end), CUBIC three, CLOSE none. A subpath is closed by a line back to
 * its MOVE whether or not it ends in CLOSE.
 */
enum : uint8_t {
    PATH_MOVE = 0,
    PATH_LINE = 1,
    PATH_QUAD = 2,
    PATH_CUBIC = 3,
    PATH_CLOSE = 4,
};

struct GfxPoint {
    int32_t x, y;
};

struct GfxPath {
    struct GfxPoint *gp_Points;
    uint8_t *gp_Verbs;
    uint32_t gp_MaxPoints, gp_MaxVerbs;
    uint32_t gp_NumPoints, gp_NumVerbs;
};

/* Coverage, a byte a pixel (A8): 0 none, 255 all. Pixel (0, 0) is the
 * path's pixel (gm_X, gm_Y). */
struct GfxMask {
    uint8_t *gm_Pixels;
    uint32_t gm_Width, gm_Height;
    uint32_t gm_BytesPerRow;
    int32_t gm_X, gm_Y;
};

/* PathMask's flags */
constexpr uint32_t PATHF_EVENODD = 1u << 0;     /* even-odd; else non-zero winding */

/* FillGradient's: from gg_From at one edge of gg_Rect to gg_To at the
 * other, top to bottom or (GRADF_HORIZONTAL) left to right, each
 * channel by a straight line in sRGB; beyond the edges, the edge's. */
struct GfxGradient {
    struct GfxRect gg_Rect;
    uint32_t gg_From, gg_To;    /* pixels */
    uint32_t gg_Flags;          /* GRADF_* */
};

constexpr uint32_t GRADF_HORIZONTAL = 1u << 0;

struct GlyphRun;                /* planar/font.h */

/* BltBitMap's blend */
enum : uint32_t {
    BLEND_SRC = 0,              /* the source's pixels, as they are */
    BLEND_SRC_OVER = 1,         /* an ARGB source over the destination, taken as opaque */
};

/*
 * gfx.library's "main" interface, version 1 (V7 step 2): every call, in
 * this order, which is the ABI. Calls are added at its end, as V8's are
 * (Data's size says how far a library's goes). A program opens it:
 *
 *     struct Library *lib = IExec->OpenLibrary("gfx.library", 7);
 *     struct GfxIFace *IGfx =
 *         lib ? (struct GfxIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
 *
 * The functions' own names are the library's: lib/gfx, for its tests.
 */
struct GfxIFace {
    struct Interface Data;

    struct BitMap *(*AllocBitMap)(uint32_t width, uint32_t height, uint32_t format,
                                  uint32_t flags, const struct BitMap *friend_bm);
    void (*FreeBitMap)(struct BitMap *bm);
    uint64_t (*GetBitMapAttr)(const struct BitMap *bm, uint32_t attr);
    bool (*InitBitMap)(struct BitMap *bm, void *base, uint32_t width, uint32_t height,
                       uint32_t bytesPerRow, uint32_t format);
    void (*InitRastPort)(struct RastPort *rp, struct BitMap *bm);

    void (*RectFill)(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax);
    void (*BltBitMap)(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
                      int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend);
    void (*DrawBevel)(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
                      uint32_t light, uint32_t dark);
    void (*Text)(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count);

    /* V8 step 1 */
    bool (*PathBounds)(const struct GfxPath *path, struct GfxRect *r);
    uint64_t (*PathMaskScratch)(uint32_t width, uint32_t height);
    bool (*PathMask)(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
                     uint64_t scratchSize);

    /* V8 step 3 */
    void (*BltMask)(struct RastPort *rp, const struct GfxMask *mask, int32_t x, int32_t y);
    void (*FillPath)(struct RastPort *rp, const struct GfxPath *path, uint32_t flags);
    void (*FillGradient)(struct RastPort *rp, const struct GfxGradient *g, const struct GfxPath *path,
                         uint32_t flags);
    bool (*PathRoundRect)(struct GfxPath *path, const struct GfxRect *r, int32_t radius);
    void (*InitTmpRas)(struct TmpRas *tr, void *buffer, uint64_t size);
    struct TextFont *(*OpenTextFont)(const struct TextAttr *ta);
    void (*CloseTextFont)(struct TextFont *tf);
    void (*SetFont)(struct RastPort *rp, const struct TextFont *tf);
    int32_t (*TextLength)(struct RastPort *rp, const char *string, uint64_t count);
    void (*TextRun)(struct RastPort *rp, const struct GlyphRun *run, uint32_t first, uint32_t count,
                    int32_t x, int32_t y);
};

/* lib/gfx's own names, which the interface's scalar entries are. */
struct BitMap *AllocBitMap(uint32_t width, uint32_t height, uint32_t format, uint32_t flags,
                           const struct BitMap *friend_bm);
void FreeBitMap(struct BitMap *bm);
uint64_t GetBitMapAttr(const struct BitMap *bm, uint32_t attr);
bool InitBitMap(struct BitMap *bm, void *base, uint32_t width, uint32_t height,
                uint32_t bytesPerRow, uint32_t format);
void InitRastPort(struct RastPort *rp, struct BitMap *bm);
void RectFill(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax);
void BltBitMap(const struct BitMap *src, int32_t xSrc, int32_t ySrc, struct RastPort *dst,
               int32_t xDest, int32_t yDest, int32_t xSize, int32_t ySize, uint32_t blend);
void DrawBevel(struct RastPort *rp, int32_t xMin, int32_t yMin, int32_t xMax, int32_t yMax,
               uint32_t light, uint32_t dark);
/* 'count' bytes at (x, y), the line's top left: in a bitmap font a
 * byte a glyph; in an outline font, UTF-8 shaped (kerned, and ligated
 * in System Sans) in pieces of up to 256 bytes, cut between characters,
 * the baseline at y + tf_Baseline. JAM2 fills the line's box first. */
void Text(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count);
/* The pixels the path's points touch, curves' controls among them:
 * false, and *r alone, if it has none or they span no area. */
bool PathBounds(const struct GfxPath *path, struct GfxRect *r);
/* Bytes of scratch PathMask needs for a mask of this size. */
uint64_t PathMaskScratch(uint32_t width, uint32_t height);
/* The path's coverage of each of the mask's pixels, within 2/255 of the
 * exact (curves taken as lines 1/512 px from them): false if its verbs and
 * points don't agree, or the scratch is short. Nothing outside the mask
 * is touched; parts of the path outside it count as they would. */
bool PathMask(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
              uint64_t scratchSize);
/* The mask's coverage blended in the foreground pen, its pixel (i, j)
 * at (x + gm_X + i, y + gm_Y + j): in linear light (docs §14.3.4), each
 * colour channel to 16 bits through a table, mixed, and back through
 * another; alpha mixed as it is. Coverage 0 leaves a pixel, 255 is the
 * pen. Within 1/255 of the exact. */
void BltMask(struct RastPort *rp, const struct GfxMask *mask, int32_t x, int32_t y);
/* The path filled in the foreground pen (PATHF_EVENODD, or non-zero),
 * its edges anti-aliased: PathMask's coverage, blended as BltMask's. */
void FillPath(struct RastPort *rp, const struct GfxPath *path, uint32_t flags);
/* The gradient over its rectangle, or, with a path, over the path
 * (filled as FillPath fills it, with 'flags'). */
void FillGradient(struct RastPort *rp, const struct GfxGradient *g, const struct GfxPath *path,
                  uint32_t flags);
/* A subpath added to 'path': the rectangle's pixels, its corners
 * rounded by 'radius' (26.6 px, cut to half the shorter side; 0 square).
 * False, and the path as it was, if its arrays are full. */
bool PathRoundRect(struct GfxPath *path, const struct GfxRect *r, int32_t radius);
void InitTmpRas(struct TmpRas *tr, void *buffer, uint64_t size);
/* One of font.library's system fonts at a size, in an Area of the
 * caller's that holds its glyph cache: nullptr if there's no such font,
 * no memory, or no font.library. A font is one task's at a time, as a
 * RastPort is. */
struct TextFont *OpenTextFont(const struct TextAttr *ta);
void CloseTextFont(struct TextFont *tf);
/* rp_Font = tf: nullptr is the 8×8. */
void SetFont(struct RastPort *rp, const struct TextFont *tf);
/* Pixels Text would advance: the shaped width, rounded, in an outline font. */
int32_t TextLength(struct RastPort *rp, const char *string, uint64_t count);
/* Glyphs first to first + count of a shaped run (font.library's ShapeRun,
 * in rp_Font's face) in the foreground pen, its pen starting at x, on
 * the baseline y. Hidden glyphs aren't drawn. Nothing if rp_Font isn't
 * an outline font. */
void TextRun(struct RastPort *rp, const struct GlyphRun *run, uint32_t first, uint32_t count,
             int32_t x, int32_t y);

#endif /* PLANAR_GFX_H */
