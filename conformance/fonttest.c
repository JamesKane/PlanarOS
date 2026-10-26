/*
 * conformance/fonttest.c -- font.library through its interface (V8.1)
 *
 * IFont and IGfx from OpenLibrary. Each system font opened from the
 * library's own constants, its names and metrics read, then
 * conformance/fontcheck.h's line drawn: each glyph's outline from IFont,
 * its coverage from IGfx's PathMask, and the CRC of it all the one the
 * host's oracle got. So outlines and coverage are the same bytes on
 * RVV, NEON and the host. Then the errors a caller can meet: a font
 * that's no font, CFF, a glyph past the last, a path too small. Exits
 * 0, or the step that failed.
 */
#include <planar/font.h>

#include "fontcheck.h"
#include "log.h"

#define CHECK(step, cond) \
    if (!(cond)) \
        return (step)

static struct FontIFace *IFont;
static struct GfxIFace *IGfx;

static int open_libs(void)
{
    struct Library *font, *gfx;

    CHECK(1, (font = IExec->OpenLibrary("font.library", 8)));
    CHECK(2, font->lib_Version == 8 && !IExec->OpenLibrary("font.library", 9));
    IFont = (struct FontIFace *)IExec->GetInterface(font, "main", 1);
    CHECK(3, IFont && IFont->Data.if_Size == sizeof(struct FontIFace));
    CHECK(4, (gfx = IExec->OpenLibrary("gfx.library", 7)));
    IGfx = (struct GfxIFace *)IExec->GetInterface(gfx, "main", 1);
    CHECK(5, IGfx && IGfx->Data.if_Size == sizeof(struct GfxIFace));
    return 0;
}

static int names(void)
{
    struct FontFace face;
    struct FontMetrics m;
    struct Span file;
    char name[32];

    CHECK(10, IFont->SystemFont(FONT_SANS, &file) && IFont->OpenFont(&face, file) == 0);
    CHECK(11, IFont->FontName(&face, FONTNAME_FAMILY, name, sizeof name) == 5 && name[0] == 'I' &&
                  name[4] == 'r');                  /* "Inter" */
    IFont->FontInfo(&face, &m);
    CHECK(12, m.fm_UnitsPerEm == 2048 && m.fm_Ascender > 0 && m.fm_Descender < 0 && !m.fm_FixedPitch);
    CHECK(13, IFont->SystemFont(FONT_MONO | FONT_BOLD, &file) && IFont->OpenFont(&face, file) == 0);
    IFont->FontInfo(&face, &m);
    CHECK(14, m.fm_UnitsPerEm == 1000 && m.fm_FixedPitch && m.fm_Weight == 700);
    CHECK(15, IFont->MapCodepoint(&face, 0x2502) != 0 && IFont->MapCodepoint(&face, 0x10ffff) == 0);
    CHECK(16, !IFont->SystemFont(FONT_BOLD | 7, &file));
    return 0;
}

static int errors(void)
{
    static const uint8_t junk[64] = { 0, 1, 0, 0, 0, 1 };
    static const uint8_t cff[12] = { 'O', 'T', 'T', 'O' };
    struct GfxPoint pts[4];
    uint8_t verbs[4];
    struct GfxPath tiny = { pts, verbs, 4, 4, 0, 0 };
    struct FontFace face;
    struct FontMetrics m;
    struct Span file;

    CHECK(20, IFont->OpenFont(&face, (struct Span){ junk, sizeof junk }) == ERR_BADFONT);
    CHECK(21, IFont->OpenFont(&face, (struct Span){ cff, sizeof cff }) == ERR_UNSUPPORTED);
    CHECK(22, IFont->SystemFont(FONT_SANS, &file) && IFont->OpenFont(&face, file) == 0);
    IFont->FontInfo(&face, &m);
    CHECK(23, IFont->GlyphOutline(&face, m.fm_NumGlyphs, 16 << 6, &tiny) == ERR_BADARG);
    CHECK(24, IFont->GlyphOutline(&face, IFont->MapCodepoint(&face, '&'), 16 << 6, &tiny) == ERR_FULL &&
                  tiny.gp_NumVerbs == 0);
    return 0;
}

int main(void)
{
    struct fontcheck_calls calls;
    int r;

    if ((r = open_libs()) || (r = names()) || (r = errors()))
        return r;
    calls = (struct fontcheck_calls){ IFont->SystemFont, IFont->OpenFont, IFont->MapCodepoint, IFont->GlyphOutline,
                                      IGfx->PathBounds, IGfx->PathMask };
    for (unsigned i = 0; i < sizeof fontcheck_fonts / sizeof fontcheck_fonts[0]; i++) {
        uint32_t crc = fontcheck_run(&calls, fontcheck_fonts[i]);

        if (crc != fontcheck_crcs[i]) {
            KPrintF("fonttest: font %u's CRC is %u, the host's %u", fontcheck_fonts[i], crc, fontcheck_crcs[i]);
            return 30 + (int)i;
        }
    }
    KPrintF("fonttest: the system fonts' glyphs at 13 px drawn as on the host, through IFont and IGfx");
    return 0;
}
