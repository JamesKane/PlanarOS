/*
 * conformance/typetest.c -- gfx.library's paths and type through its
 * interface (V8.3)
 *
 * IGfx from OpenLibrary, its step 3 calls there (the interface's size
 * says so), then conformance/typecheck.h's cases through it: masks,
 * paths, gradients and text against the reference, and against
 * lib/gfx's scalar entries, linked in, to the bit. On QEMU's CPUs the
 * table's are the vector unit's (RVV, NEON), so this is where RVV's
 * sweep and blend are checked. gfx.library finds font.library itself,
 * through the process data: this program opens it only to shape a run
 * for TextRun. Then the scene, by the table and by the scalar entries:
 * its CRC must be the host's. Then the costs: a glyph drawn into the
 * cache, a line of 80 from it, and a filled path; and (V8 step 8) a
 * 1920 by 1080 screen of the Shell's text, System Mono 13 px as conwin
 * draws it, each line cleared and drawn. Exits 0, or the step that
 * failed.
 */
#include <planar/font.h>
#include <planar/gfx.h>

#include "log.h"
#include "typecheck.h"

#define CHECK(step, cond) \
    if (!(cond)) \
        return (step)

static struct GfxIFace *IGfx;

static const struct GfxIFace scalar = {
    .AllocBitMap = AllocBitMap, .FreeBitMap = FreeBitMap, .GetBitMapAttr = GetBitMapAttr,
    .InitBitMap = InitBitMap, .InitRastPort = InitRastPort,
    .RectFill = RectFill, .BltBitMap = BltBitMap, .DrawBevel = DrawBevel, .Text = Text,
    .PathBounds = PathBounds, .PathMaskScratch = PathMaskScratch, .PathMask = PathMask,
    .BltMask = BltMask, .FillPath = FillPath, .FillGradient = FillGradient,
    .PathRoundRect = PathRoundRect, .InitTmpRas = InitTmpRas, .OpenTextFont = OpenTextFont,
    .CloseTextFont = CloseTextFont, .SetFont = SetFont, .TextLength = TextLength, .TextRun = TextRun,
};

static int open_libs(void)
{
    struct Library *gfx, *font;

    CHECK(1, (gfx = IExec->OpenLibrary("gfx.library", 8)) && gfx->lib_Revision >= 3);
    IGfx = (struct GfxIFace *)IExec->GetInterface(gfx, "main", 1);
    CHECK(2, IGfx && IGfx->Data.if_Size == sizeof(struct GfxIFace) && IGfx->TextRun);
    CHECK(3, (font = IExec->OpenLibrary("font.library", 8)));
    tc_ifont = (const struct FontIFace *)IExec->GetInterface(font, "main", 1);
    CHECK(4, tc_ifont != nullptr);
    return 0;
}

/* The scene by 'g', and its CRC. */
static int scene(const struct GfxIFace *g, uint32_t *crc)
{
    struct BitMap *bm;
    struct RastPort rp;

    CHECK(20, (bm = IGfx->AllocBitMap(TC_SCENE_W, TC_SCENE_H, PIXFMT_XRGB8888, 0, nullptr)));
    IGfx->InitRastPort(&rp, bm);
    CHECK(21, tc_scene(g, &rp));
    *crc = tc_crc(bm);
    IGfx->FreeBitMap(bm);
    return 0;
}

/* A 1920 by 1080 screen of the Shell's text, every cell a character,
 * as conwin draws a line: its background, then its text (JAM1). The
 * glyphs are cached first, as a Shell's soon are. */
static int screenful(void)
{
    static char line[1920];
    struct BitMap *bm;
    struct RastPort rp;
    struct TextFont *tf;
    uint32_t cols, rows;
    uint64_t t0, t1;

    CHECK(50, (bm = IGfx->AllocBitMap(1920, 1080, PIXFMT_XRGB8888, 0, nullptr)));
    IGfx->InitRastPort(&rp, bm);
    rp.rp_DrawMode = JAM1;
    CHECK(51, (tf = IGfx->OpenTextFont(&(struct TextAttr){ "System Mono", 13, 0, 0 })) && tf->tf_XSize);
    IGfx->SetFont(&rp, tf);
    cols = 1920 / tf->tf_XSize;
    rows = 1080 / tf->tf_YSize;
    for (uint32_t i = 0; i < cols; i++)
        line[i] = (char)('!' + i % 94);
    IGfx->Text(&rp, 0, 0, line, 94);
    t0 = CurrentTime();
    for (uint32_t r = 0; r < rows; r++) {
        int32_t y = (int32_t)(r * tf->tf_YSize);

        rp.rp_FgPen = 0xff101820;
        IGfx->RectFill(&rp, 0, y, 1919, y + tf->tf_YSize - 1);
        rp.rp_FgPen = 0xffd8dee9;
        IGfx->Text(&rp, 0, y, line + r % 7, cols - 7);
    }
    t1 = CurrentTime();
    KPrintF("typetest: a 1920x1080 screen of the Shell's text, %lu lines of %lu in System Mono 13 px: %lu us",
            (uint64_t)rows, (uint64_t)(cols - 7), (t1 - t0) / 1000);
    IGfx->CloseTextFont(tf);
    IGfx->FreeBitMap(bm);
    return 0;
}

/* Microseconds: a glyph into the cache, and a line of 80 from it; a
 * 200 px rounded rectangle filled, and its mask's blend. */
static int costs(void)
{
    static char line[80];
    static struct GfxPoint pts[24];
    static uint8_t verbs[24];
    struct GfxPath path = { pts, verbs, 24, 24, 0, 0 };
    struct BitMap *bm;
    struct RastPort rp;
    struct TextFont *tf;
    uint64_t t[4], glyph, cached, fill;

    CHECK(40, (bm = IGfx->AllocBitMap(640, 240, PIXFMT_XRGB8888, 0, nullptr)));
    IGfx->InitRastPort(&rp, bm);
    rp.rp_DrawMode = JAM1;
    CHECK(41, (tf = IGfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 })));
    IGfx->SetFont(&rp, tf);
    for (unsigned i = 0; i < sizeof line; i++)
        line[i] = (char)('!' + i % 94);
    t[0] = CurrentTime();
    IGfx->Text(&rp, 0, 0, line, 80);                 /* 80 glyphs, each drawn into the cache */
    t[1] = CurrentTime();
    for (unsigned i = 0; i < 8; i++)
        IGfx->Text(&rp, 0, 20, line, 80);            /* the same phases: from the cache */
    t[2] = CurrentTime();
    IGfx->PathRoundRect(&path, &(struct GfxRect){ 10, 20, 209, 219 }, 24 * 64);
    for (unsigned i = 0; i < 8; i++)
        IGfx->FillPath(&rp, &path, 0);
    t[3] = CurrentTime();
    glyph = (t[1] - t[0]) / 80 / 1000;
    cached = (t[2] - t[1]) / 8 / 1000;
    fill = (t[3] - t[2]) / 8 / 1000;
    KPrintF("typetest: System Sans 13 px: %lu us a glyph drawn into the cache; a line of 80 from it "
            "%lu us; a 200 px rounded rectangle filled %lu us", glyph, cached, fill);
    IGfx->CloseTextFont(tf);
    IGfx->FreeBitMap(bm);
    return screenful();
}

int main(void)
{
    unsigned cases = 0, line;
    uint32_t by_table, by_scalar;
    int r;

    if ((r = open_libs()))
        return r;
    if ((line = tc_all(IGfx, &scalar, &cases))) {
        KPrintF("typetest: typecheck.h:%u failed (case %d, %d, %d)", line, gc_w, gc_a, gc_p);
        return 10;
    }
    if ((r = scene(IGfx, &by_table)) || (r = scene(&scalar, &by_scalar)))
        return r;
    if (by_table != tc_scene_crc || by_scalar != tc_scene_crc) {
        KPrintF("typetest: the scene's CRC %08lx by the table, %08lx scalar, not the host's %08lx",
                (uint64_t)by_table, (uint64_t)by_scalar, (uint64_t)tc_scene_crc);
        return 30;
    }
    if ((r = costs()))
        return r;
    KPrintF("typetest: %u cases through IGfx as the reference and the scalar entries draw them; "
            "the scene %08lx, as the host's", cases, (uint64_t)by_table);
    return 0;
}
