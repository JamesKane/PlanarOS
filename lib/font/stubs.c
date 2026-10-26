/*
 * lib/font/stubs.c -- what font.library doesn't do, and does instead (§14.4)
 *
 * One function for each subsystem §14.4 leaves out, with the signature
 * a real one would have, doing the fallback §14.4 gives and returning
 * its warning or error. Building one of them is replacing its function
 * here, and adding a pass for it to tools/font-oracle.
 */
#include "sfnt.h"

/* Complex scripts: GSUB's contextual lookups, GPOS's marks and cursive
 * attachment, Indic and Arabic clusters. The run stays as cmap mapped
 * it, one glyph a character, with hmtx's advances. */
uint32_t font_shape_complex_stub(const struct font *, struct GlyphRun *)
{
    return FONT_WARN_STUB_SHAPER;
}

/* UAX #9's reordering: the run stays in logical order, left to right. */
uint32_t font_bidi_reorder_stub(struct GlyphRun *)
{
    return FONT_WARN_STUB_BIDI;
}

/* fvar, gvar, HVAR and the rest: the font's default instance is used. */
uint32_t font_apply_variations_stub(const struct font *)
{
    return FONT_WARN_STUB_VAR;
}

/* COLR, CBDT, sbix, SVG: no colour glyph, so the caller draws glyf's
 * outline (FONT_WARN_STUB_COLOR). */
int64_t font_raster_color_stub(const struct font *, uint32_t, uint32_t)
{
    return ERR_UNSUPPORTED;
}

/* vhea, vmtx, VORG: no vertical layout. */
int64_t font_vertical_metrics_stub(const struct font *, uint32_t, struct GlyphMetrics *)
{
    return ERR_UNSUPPORTED;
}

/* TrueType's instructions: never run; outlines are used unhinted. */
void font_tt_hint_glyph_stub(const struct font *, uint32_t, uint32_t)
{
}
