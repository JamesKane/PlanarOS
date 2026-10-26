/*
 * conformance/fontcheck.h -- glyphs drawn the same everywhere (V8.1)
 *
 * A line of characters from each system font, each glyph's outline at
 * 13 px turned into coverage, and a CRC-32 over the boxes and the bytes.
 * The rasteriser is integer arithmetic, so the host and both ISAs must
 * get the same: tools/font-oracle checks these CRCs on the host, against
 * a FreeType it has already held the glyphs to, and conformance/fonttest
 * checks them through IFont and IGfx on QEMU. A change to the outlines
 * or the rasteriser changes them: make font-oracle says the new ones.
 */
#ifndef CONFORMANCE_FONTCHECK_H
#define CONFORMANCE_FONTCHECK_H

#include <planar/font.h>

struct fontcheck_calls {
    bool (*SystemFont)(uint32_t which, struct Span *file);
    int64_t (*OpenFont)(struct FontFace *face, struct Span file);
    uint32_t (*MapCodepoint)(const struct FontFace *face, uint32_t codepoint);
    int64_t (*GlyphOutline)(const struct FontFace *face, uint32_t glyph, uint32_t size, struct GfxPath *path);
    bool (*PathBounds)(const struct GfxPath *path, struct GfxRect *r);
    bool (*PathMask)(const struct GfxPath *path, uint32_t flags, struct GfxMask *mask, void *scratch,
                     uint64_t scratchSize);
};

static const uint32_t fontcheck_fonts[] = { FONT_SANS, FONT_SANS | FONT_BOLD, FONT_MONO, FONT_MONO | FONT_BOLD };

/* "Planar 8.1 (fi) & ÁÉ —│", and each font's CRC of it. */
static const uint32_t fontcheck_text[] = {
    'P', 'l', 'a', 'n', 'a', 'r', ' ', '8', '.', '1', ' ', '(', 'f', 'i', ')', ' ', '&', ' ', 0xc1, 0xc9, ' ',
    0x2014, 0x2502,
};
static const uint32_t fontcheck_crcs[] = { 0x261e5138, 0xcc281536, 0xed247480, 0x4650b830 };

static uint32_t fontcheck_crc32(uint32_t crc, const void *p, uint64_t n)
{
    const uint8_t *b = p;

    for (uint64_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
    }
    return crc;
}

/* The CRC of fontcheck_text in font 'which', or 0 if a call failed. */
static uint32_t fontcheck_run(const struct fontcheck_calls *c, uint32_t which)
{
    static struct GfxPoint pts[1024];
    static uint8_t verbs[1024];
    static uint8_t mask[64 * 64];
    static int32_t scratch[66 * 64];
    struct GfxPath path = { pts, verbs, 1024, 1024, 0, 0 };
    struct FontFace face;
    struct Span file;
    uint32_t crc = ~0u;

    if (!c->SystemFont(which, &file) || c->OpenFont(&face, file) != 0)
        return 0;
    for (unsigned i = 0; i < sizeof fontcheck_text / sizeof fontcheck_text[0]; i++) {
        uint32_t g = c->MapCodepoint(&face, fontcheck_text[i]);
        struct GfxRect r;
        int32_t box[4];

        if (c->GlyphOutline(&face, g, 13 << 6, &path) != 0)
            return 0;
        if (!c->PathBounds(&path, &r))
            continue;                   /* a space */
        box[0] = r.MinX, box[1] = r.MinY, box[2] = r.MaxX - r.MinX + 1, box[3] = r.MaxY - r.MinY + 1;
        if (box[2] > 64 || box[3] > 64)
            return 0;
        struct GfxMask m = { mask, (uint32_t)box[2], (uint32_t)box[3], (uint32_t)box[2], r.MinX, r.MinY };

        if (!c->PathMask(&path, 0, &m, scratch, sizeof scratch))
            return 0;
        crc = fontcheck_crc32(crc, box, sizeof box);
        crc = fontcheck_crc32(crc, mask, (uint64_t)box[2] * (uint64_t)box[3]);
    }
    return ~crc;
}

#endif /* CONFORMANCE_FONTCHECK_H */
