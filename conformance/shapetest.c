/*
 * conformance/shapetest.c -- font.library's shaping through its interface (V8.2)
 *
 * IFont from OpenLibrary, its step 2 calls there (the interface's size
 * says so). Each system font shapes conformance/fontcheck.h's text,
 * twice, and breaks it into lines, and the CRC of it all must be the
 * one the host's oracle got, where pass 2 holds ShapeRun to HarfBuzz.
 * Then what a caller can meet: the kerning in System Sans and none in
 * System Mono, a ligature off in Mono and tabular figures in Sans, the
 * controls hidden, ERR_FULL, and a Hebrew run's warnings. Exits 0, or
 * the step that failed.
 */
#include <planar/font.h>

#include "fontcheck.h"
#include "log.h"

#define CHECK(step, cond) \
    if (!(cond)) \
        return (step)

static struct FontIFace *IFont;
static struct ShapedGlyph glyphs[64];

static int32_t width(const struct FontFace *face, const char *text, uint32_t features)
{
    struct GlyphRun run = { glyphs, 64, 0, 0, 0 };
    uint64_t len = 0;

    while (text[len])
        len++;
    return IFont->ShapeRun(face, text, len, features, &run) == 0 ? run.gr_Advance : -1;
}

static int calls(void)
{
    struct FontFace sans, mono;
    struct Span file;
    struct GlyphRun run = { glyphs, 64, 0, 0, 0 };

    CHECK(10, IFont->SystemFont(FONT_SANS, &file) && IFont->OpenFont(&sans, file) == 0);
    CHECK(11, IFont->SystemFont(FONT_MONO, &file) && IFont->OpenFont(&mono, file) == 0);
    CHECK(12, width(&sans, "AV", FONT_FEAT_KERN) < width(&sans, "AV", 0));
    CHECK(13, width(&mono, "AV", FONT_FEAT_KERN) == width(&mono, "AV", 0));
    CHECK(14, width(&sans, "1111", FONT_FEAT_TNUM) != width(&sans, "1111", 0));
    CHECK(15, IFont->ShapeRun(&sans, "a\tb\r\n", 5, FONT_FEAT_KERN, &run) == 0 && run.gr_Count == 5 &&
                  run.gr_Glyphs[1].sg_Flags == (GLYPHF_TAB | GLYPHF_HIDDEN) && run.gr_Glyphs[1].sg_Advance == 0 &&
                  run.gr_Glyphs[3].sg_Flags == GLYPHF_HIDDEN &&
                  run.gr_Glyphs[4].sg_Flags == (GLYPHF_NEWLINE | GLYPHF_HIDDEN));
    run.gr_Max = 2;
    CHECK(16, IFont->ShapeRun(&sans, "abc", 3, 0, &run) == ERR_FULL && run.gr_Count == 0);
    run.gr_Max = 64;
    CHECK(17, IFont->ShapeRun(&sans, "\xd7\xa9\xd7\x9c", 4, FONT_FEAT_KERN, &run) == 0 && run.gr_Count == 2 &&
                  run.gr_Warnings == (FONT_WARN_STUB_SHAPER | FONT_WARN_STUB_BIDI));
    return 0;
}

int main(void)
{
    struct Library *font;
    int r;

    CHECK(1, (font = IExec->OpenLibrary("font.library", 8)));
    CHECK(2, font->lib_Revision >= 2);
    IFont = (struct FontIFace *)IExec->GetInterface(font, "main", 1);
    CHECK(3, IFont && IFont->Data.if_Size == sizeof(struct FontIFace) && IFont->LineSpacing);
    if ((r = calls()))
        return r;

    struct shapecheck_calls c = { IFont->SystemFont, IFont->OpenFont, IFont->ShapeRun, IFont->FitLine,
                                  IFont->LineSpacing };

    for (unsigned i = 0; i < sizeof fontcheck_fonts / sizeof fontcheck_fonts[0]; i++) {
        uint32_t crc = shapecheck_run(&c, fontcheck_fonts[i]);

        if (crc != shapecheck_crcs[i]) {
            KPrintF("shapetest: font %u's shaping CRC is %u, the host's %u", fontcheck_fonts[i], crc,
                    shapecheck_crcs[i]);
            return 30 + (int)i;
        }
    }
    KPrintF("shapetest: the system fonts' text shaped and broken into lines as on the host, through IFont");
    return 0;
}
