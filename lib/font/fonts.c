/*
 * lib/font/fonts.c -- the system fonts, in the library's image (V8 step 1)
 *
 * System Sans is Inter and System Mono JetBrains Mono, Regular and Bold,
 * each cut to docs §14.3.1's repertoire by tools/font-subset.py (v8-plan
 * decision 2). Both are under the SIL Open Font License 1.1, whose text
 * is beside the originals in tools/font-oracle/corpus/. They're the
 * library's constants, so every process maps one copy (§14.1's zero
 * copy, with no file to load).
 */
#include <planar/font.h>

static const uint8_t sans[] = {
#embed "fonts/SystemSans-Regular.ttf"
};
static const uint8_t sans_bold[] = {
#embed "fonts/SystemSans-Bold.ttf"
};
static const uint8_t mono[] = {
#embed "fonts/SystemMono-Regular.ttf"
};
static const uint8_t mono_bold[] = {
#embed "fonts/SystemMono-Bold.ttf"
};

bool SystemFont(uint32_t which, struct Span *file)
{
    switch (which) {
    case FONT_SANS: *file = (struct Span){ sans, sizeof sans }; return true;
    case FONT_SANS | FONT_BOLD: *file = (struct Span){ sans_bold, sizeof sans_bold }; return true;
    case FONT_MONO: *file = (struct Span){ mono, sizeof mono }; return true;
    case FONT_MONO | FONT_BOLD: *file = (struct Span){ mono_bold, sizeof mono_bold }; return true;
    }
    return false;
}
