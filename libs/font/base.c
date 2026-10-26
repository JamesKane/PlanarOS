/*
 * libs/font/base.c -- font.library: its base and its interface
 *
 * V8 step 1. lib/font built as a shared library image (lib.ld), with
 * utility's checked readers linked in, and the system fonts among its
 * constants (lib/font/fonts.c). It keeps no state: a FontFace is the
 * caller's. Its outlines are drawn by gfx.library (PathMask).
 */
#include <planar/font.h>
#include <planar/libimage.h>

static const struct Library base;

static const struct FontIFace main_iface = {
    .Data = { "main", 1, sizeof(struct FontIFace), (struct Library *)&base },

    .SystemFont = SystemFont,
    .OpenFont = OpenFont,
    .FontInfo = FontInfo,
    .FontName = FontName,
    .MapCodepoint = MapCodepoint,
    .GetGlyphMetrics = GetGlyphMetrics,
    .GlyphOutline = GlyphOutline,
    .ScaleUnits = ScaleUnits,
};

static struct Interface *const interfaces[] = { (struct Interface *)&main_iface, nullptr };

static const struct Library base = {
    .lib_Name = "font.library",
    .lib_Version = 8,
    .lib_Revision = 1,
    .lib_IdString = "font.library 8.1 (26.10.2026)",
    .lib_Interfaces = interfaces,
};

const struct LibraryImage _library_image = {
    .li_Library = (struct Library *)&base,
};
