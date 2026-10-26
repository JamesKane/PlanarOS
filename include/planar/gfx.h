/*
 * planar/gfx.h -- gfx.library: bitmaps, rastports and drawing on the CPU
 *
 * V7 step 2 (docs §16.3 pillar 1, §16.4). A BitMap describes 32-bit
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

/* A font of glyphs at most 8 pixels wide: a byte a row, bit 0 leftmost. */
struct TextFont {
    uint16_t tf_YSize, tf_XSize;
    uint16_t tf_LoChar, tf_HiChar;          /* the glyphs held; others are tf_LoChar's */
    const uint8_t *tf_CharData;             /* tf_YSize bytes a glyph, from tf_LoChar */
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

#endif /* PLANAR_GFX_H */
