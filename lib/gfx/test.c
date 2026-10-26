/*
 * lib/gfx/test.c -- gfx.library on the host
 *
 * lib/gfx's files with the sanitizers, against conformance/gfxcheck.h's
 * and typecheck.h's references, through two tables: the scalar entries,
 * and, where the host is an Arm one (GFX_TEST_NEON), arch/aarch64/lib/
 * gfx.c's NEON entries. RVV's are checked in the guest, by
 * conformance/gfx.c and typetest.c: macOS has no QEMU user mode to run
 * them here. Areas are the host's memory, page aligned and zeroed, and
 * every one must be freed by the end. font.library is lib/font, linked
 * in, as a table (gfx_host_ifont).
 *
 * Then BltMask's blend against the exact, in floating point: every
 * colour, under every colour, at every coverage, within 1/255. And the
 * scene's CRC, which typecheck.h holds the guest to.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rows.h"
#include "typecheck.h"

extern const struct FontIFace *gfx_host_ifont;

static struct {
    void *base;
    uint64_t size;
    uint32_t flags;
} areas[16];
static unsigned live;

int64_t AllocArea(uint64_t size, uint32_t flags, void **address)
{
    size = (size + 4095) & ~4095ull;
    for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++)
        if (!areas[i].base) {
            if (!(areas[i].base = aligned_alloc(4096, size)))
                return ERR_NOMEM;
            memset(areas[i].base, 0, size);
            areas[i].size = size;
            areas[i].flags = flags;
            *address = areas[i].base;
            live++;
            return i + 1;
        }
    return ERR_NOMEM;
}

int64_t FreeArea(Handle h)
{
    if (h < 1 || h > sizeof areas / sizeof areas[0] || !areas[h - 1].base)
        return ERR_BADHANDLE;
    free(areas[h - 1].base);
    areas[h - 1].base = nullptr;
    live--;
    return 0;
}

int64_t QueryArea(const void *address, struct AreaInfo *info)
{
    for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++) {
        uint8_t *b = areas[i].base;

        if (b && (const uint8_t *)address >= b && (const uint8_t *)address < b + areas[i].size) {
            *info = (struct AreaInfo){ .ai_Address = b, .ai_Size = areas[i].size, .ai_Area = i + 1,
                                       .ai_Rights = AREAR_READ | AREAR_WRITE, .ai_Flags = areas[i].flags,
                                       .ai_Sharers = 1 };
            return 0;
        }
    }
    return ERR_NOTFOUND;
}

static int run(const char *name, const struct GfxIFace *g, const struct GfxIFace *ref)
{
    unsigned cases = 0, line = gc_all(g, &cases);

    if (line) {
        printf("gfx-test: FAILED: %s: gfxcheck.h:%u (width %d, at %d, place %d)\n", name, line, gc_w,
               gc_a, gc_p);
        return 1;
    }
    if ((line = tc_all(g, ref, &cases))) {
        printf("gfx-test: FAILED: %s: typecheck.h:%u (case %d, %d, %d)\n", name, line, gc_w, gc_a, gc_p);
        return 1;
    }
    if (live) {
        printf("gfx-test: FAILED: %s: %u Areas not freed\n", name, live);
        return 1;
    }
    printf("gfx-test: ok: %s: %u cases\n", name, cases);
    return 0;
}

static double to_linear(unsigned v)
{
    double c = v / 255.0;

    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double to_srgb(double l)
{
    return 255 * (l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055);
}

/* gfx_mask_px against the exact blend in linear light, every channel
 * value, under every other, at every coverage: within 1/255. */
static int blend(void)
{
    static double lin[256];
    unsigned worst = 0;
    uint64_t off = 0;

    for (unsigned v = 0; v < 256; v++)
        lin[v] = to_linear(v);
    for (unsigned c = 0; c < 256; c++)
        for (unsigned s = 0; s < 256; s++)
            for (unsigned d = 0; d < 256; d++) {
                double a = c / 255.0;
                long exact = lround(to_srgb(lin[s] * a + lin[d] * (1 - a)));
                long got = gfx_mask_px(s << 16 | s << 8 | s, d << 16 | d << 8 | d, c) & 0xff;
                unsigned e = (unsigned)labs(got - exact);

                worst = e > worst ? e : worst;
                off += e != 0;
            }
    if (worst > 1) {
        printf("gfx-test: FAILED: BltMask's blend %u/255 from the exact\n", worst);
        return 1;
    }
    printf("gfx-test: ok: BltMask's blend in linear light: 16777216 colours, every one within 1/255 of "
           "the exact, %.1f%% off by 1/255\n", 100.0 * off / 16777216);
    return 0;
}

/* The scene, by 'g': its CRC, and the right one. */
static int scene(const char *name, const struct GfxIFace *g)
{
    struct BitMap *bm = AllocBitMap(TC_SCENE_W, TC_SCENE_H, PIXFMT_XRGB8888, 0, nullptr);
    struct RastPort rp;
    uint32_t crc;

    InitRastPort(&rp, bm);
    if (!tc_scene(g, &rp)) {
        printf("gfx-test: FAILED: %s: the scene's fonts didn't open\n", name);
        return 1;
    }
    crc = tc_crc(bm);
    FreeBitMap(bm);
    if (crc != tc_scene_crc) {
        printf("gfx-test: FAILED: %s: the scene's CRC is %08x, not typecheck.h's %08x\n", name, crc,
               tc_scene_crc);
        return 1;
    }
    printf("gfx-test: ok: %s: the scene, %08x\n", name, crc);
    return 0;
}

int main(void)
{
    static const struct FontIFace font = {
        .SystemFont = SystemFont, .OpenFont = OpenFont, .FontInfo = FontInfo, .FontName = FontName,
        .MapCodepoint = MapCodepoint, .GetGlyphMetrics = GetGlyphMetrics, .GlyphOutline = GlyphOutline,
        .ScaleUnits = ScaleUnits, .ShapeRun = ShapeRun, .FitLine = FitLine, .LineSpacing = LineSpacing,
    };
    static const struct GfxIFace scalar = {
        .AllocBitMap = AllocBitMap, .FreeBitMap = FreeBitMap, .GetBitMapAttr = GetBitMapAttr,
        .InitBitMap = InitBitMap, .InitRastPort = InitRastPort,
        .RectFill = RectFill, .BltBitMap = BltBitMap, .DrawBevel = DrawBevel, .Text = Text,
        .PathBounds = PathBounds, .PathMaskScratch = PathMaskScratch, .PathMask = PathMask,
        .BltMask = BltMask, .FillPath = FillPath, .FillGradient = FillGradient,
        .PathRoundRect = PathRoundRect, .InitTmpRas = InitTmpRas, .OpenTextFont = OpenTextFont,
        .CloseTextFont = CloseTextFont, .SetFont = SetFont, .TextLength = TextLength, .TextRun = TextRun,
    };
    struct GfxIFace vector = scalar;

    gfx_host_ifont = tc_ifont = &font;
    if (blend() || run("scalar", &scalar, &scalar) || scene("scalar", &scalar))
        return 1;
#ifdef GFX_TEST_NEON
    vector.RectFill = RectFillVector;
    vector.BltBitMap = BltBitMapVector;
    vector.DrawBevel = DrawBevelVector;
    vector.Text = TextVector;
    vector.PathMask = PathMaskVector;
    vector.BltMask = BltMaskVector;
    vector.FillPath = FillPathVector;
    vector.FillGradient = FillGradientVector;
    vector.TextRun = TextRunVector;
    return run("NEON", &vector, &scalar) || scene("NEON", &vector);
#else
    (void)vector;
    printf("gfx-test: no vector entries for this host: scalar only\n");
    return 0;
#endif
}
