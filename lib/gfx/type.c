/*
 * lib/gfx/type.c -- outline fonts: OpenTextFont, CloseTextFont, SetFont,
 * TextLength, TextRun, and Text in one (V8 step 3)
 *
 * An outline font is one of font.library's system fonts at a size, in
 * an Area of the caller's: the TextFont, the face, and its glyph cache
 * (v8-plan decision 4: each process's own, and gone with it). The cache
 * keeps a glyph's coverage by glyph and subpixel phase: four phases, a
 * quarter pixel apart, for System Sans at 12 to 16 px, where whole
 * pixels make the spacing uneven, and whole pixels otherwise (VQ35). A
 * glyph is drawn into a box every glyph fits, head's bounds at this
 * size, by the table's PathMask, and kept as tall and wide as its
 * outline's points reach. When the cache is full it's emptied, and fills
 * again. The Area also holds what drawing needs, a glyph's outline and
 * PathMask's scratch, and Text's shaped glyphs, so nothing big is on
 * the stack, which is 16 KiB.
 *
 * gfx.library has no data of its own, so it finds font.library through
 * the program's IExec, which the process data holds (lib/exec/procdata.h);
 * the host's tests give it lib/font's own table instead.
 */
#include <planar/font.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

#include "rows.h"

#if !__STDC_HOSTED__
#include "../exec/procdata.h"
#endif

constexpr uint32_t SLOTS = 1024;            /* the cache's, a power of two */
constexpr uint32_t SLOTS_FULL = 768;        /* emptied at this many */
constexpr uint32_t MAX_POINTS = 4096;       /* a glyph's outline, at most; its verbs likewise */
constexpr uint32_t RUN_GLYPHS = 256;        /* Text's piece: as many as its bytes */
constexpr uint64_t ARENA_MIN = 64 * 1024, ARENA_MAX = 4 * 1024 * 1024;

struct cached {
    uint32_t key;               /* glyph * 4 + phase, plus 1: 0 is a free slot */
    int16_t x, y;               /* the coverage's top left, from the pen on the baseline */
    uint16_t w, h;              /* 0 if the glyph draws nothing */
    uint32_t at;                /* its bytes in the arena */
};

struct GfxFontCache {
    struct TextFont tf;         /* first: what OpenTextFont returns */
    const struct FontIFace *ifont;
    Handle area;
    uint32_t phases;            /* 4 or 1 */
    uint32_t features;          /* ShapeRun's, for Text */
    uint32_t used;              /* slots taken */
    int32_t bx, by;             /* the box every glyph is drawn in, from the pen */
    uint32_t bw, bh;
    uint64_t arena_size, arena_used;
    struct cached *slots;
    uint8_t *arena;
    struct GfxPoint *points;
    uint8_t *verbs;
    struct ShapedGlyph *glyphs;
    int32_t *scratch;
    uint64_t scratch_size;
    uint8_t *raster;
    struct FontFace face;
};

#if __STDC_HOSTED__
/* The host's tests set it: lib/font's functions, as a table. */
const struct FontIFace *gfx_host_ifont;

static const struct FontIFace *font_iface(void)
{
    return gfx_host_ifont;
}
#else
static const struct FontIFace *font_iface(void)
{
    struct ExecIFace *e = process_data()->pd_IExec;
    struct Library *lib = e ? e->OpenLibrary("font.library", 8) : nullptr;

    return lib ? (const struct FontIFace *)e->GetInterface(lib, "main", 1) : nullptr;
}
#endif

/* a / b rounded down, b > 0. */
static int64_t floor_div(int64_t a, int64_t b)
{
    return a / b - (a % b != 0 && a < 0);
}

static uint64_t align8(uint64_t n)
{
    return (n + 7) & ~7ull;
}

static const struct GfxFontCache *outline_of(const struct RastPort *rp)
{
    const struct TextFont *tf = rp ? rp->rp_Font : nullptr;

    return tf && (tf->tf_Flags & FPF_OUTLINE) && tf->tf_Cache ? tf->tf_Cache : nullptr;
}

struct TextFont *OpenTextFont(const struct TextAttr *ta)
{
    const struct FontIFace *f = font_iface();
    uint32_t which, size, bw, bh;
    struct GfxFontCache *c;
    struct FontFace face;
    struct FontMetrics m;
    struct LineSpacing ls;
    struct GlyphMetrics gm;
    struct Span file;
    int64_t bx, by, right, bottom, cap, h;
    uint64_t arena, total, at;
    uint8_t *base;
    void *mem;

    if (!ta || !f || ta->ta_YSize < 1 || ta->ta_YSize > 255)
        return nullptr;
    if (!ta->ta_Name || StrEqual(ta->ta_Name, "System Sans"))
        which = FONT_SANS;
    else if (StrEqual(ta->ta_Name, "System Mono"))
        which = FONT_MONO;
    else
        return nullptr;
    which |= ta->ta_Style & FSF_BOLD ? FONT_BOLD : 0;
    size = (uint32_t)ta->ta_YSize << 6;
    if (!f->SystemFont(which, &file) || f->OpenFont(&face, file) != 0)
        return nullptr;
    f->FontInfo(&face, &m);

    /* The box: head's, at this size, a pixel around, and the furthest phase. */
    cap = 4 * (int64_t)ta->ta_YSize + 8;
    bx = floor_div(f->ScaleUnits(&face, m.fm_XMin, size), 64) - 1;
    right = -floor_div(-(int64_t)f->ScaleUnits(&face, m.fm_XMax, size), 64) + 2;
    by = floor_div(-(int64_t)f->ScaleUnits(&face, m.fm_YMax, size), 64) - 1;
    bottom = -floor_div((int64_t)f->ScaleUnits(&face, m.fm_YMin, size), 64) + 1;
    bx = bx < -cap ? -cap : bx;
    by = by < -cap ? -cap : by;
    right = right > bx + cap ? bx + cap : right <= bx ? bx + 1 : right;
    bottom = bottom > by + cap ? by + cap : bottom <= by ? by + 1 : bottom;
    bw = (uint32_t)(right - bx);
    bh = (uint32_t)(bottom - by);

    arena = 32 * (uint64_t)bw * bh;
    arena = arena < ARENA_MIN ? ARENA_MIN : arena > ARENA_MAX ? ARENA_MAX : arena;
    total = align8(sizeof *c) + align8(SLOTS * sizeof(struct cached)) + align8(arena) +
            align8(MAX_POINTS * sizeof(struct GfxPoint)) + align8(MAX_POINTS) +
            align8(RUN_GLYPHS * sizeof(struct ShapedGlyph)) + align8(PathMaskScratch(bw, bh)) +
            align8((uint64_t)bw * bh);
    if ((h = AllocArea(total, 0, &mem)) < 0)
        return nullptr;
    base = mem;
    c = mem;
    at = align8(sizeof *c);
#define CARVE(field, bytes) (c->field = (void *)(base + at), at += align8(bytes))
    CARVE(slots, SLOTS * sizeof(struct cached));
    CARVE(arena, arena);
    CARVE(points, MAX_POINTS * sizeof(struct GfxPoint));
    CARVE(verbs, MAX_POINTS);
    CARVE(glyphs, RUN_GLYPHS * sizeof(struct ShapedGlyph));
    CARVE(scratch, PathMaskScratch(bw, bh));
    CARVE(raster, (uint64_t)bw * bh);
#undef CARVE
    c->ifont = f;
    c->area = (Handle)h;
    c->scratch_size = PathMaskScratch(bw, bh);
    c->arena_size = arena;
    c->bx = (int32_t)bx, c->by = (int32_t)by, c->bw = bw, c->bh = bh;
    c->phases = which == FONT_SANS || which == (FONT_SANS | FONT_BOLD) ? 4 : 1;
    c->phases = ta->ta_YSize >= 12 && ta->ta_YSize <= 16 ? c->phases : 1;
    c->features = (which & ~FONT_BOLD) == FONT_SANS ? FONT_FEAT_KERN | FONT_FEAT_LIGA : FONT_FEAT_KERN;
    if (f->OpenFont(&c->face, file) != 0) {
        FreeArea((Handle)h);
        return nullptr;
    }
    f->LineSpacing(&c->face, &ls);
    c->tf = (struct TextFont){
        .tf_YSize = (uint16_t)((f->ScaleUnits(&c->face, ls.ls_Height, size) + 32) >> 6),
        .tf_Baseline = (uint16_t)((f->ScaleUnits(&c->face, ls.ls_Ascent, size) + 32) >> 6),
        .tf_LoChar = 0,
        .tf_HiChar = 0xffff,
        .tf_Flags = FPF_OUTLINE | (m.fm_FixedPitch ? FPF_FIXED : 0),
        .tf_Size = size,
        .tf_Cache = c,
    };
    if (m.fm_FixedPitch && f->GetGlyphMetrics(&c->face, f->MapCodepoint(&c->face, 'M'), &gm) == 0)
        c->tf.tf_XSize = (uint16_t)((f->ScaleUnits(&c->face, gm.gm_Advance, size) + 32) >> 6);
    return &c->tf;
}

/* The Area is freed, the TextFont in it: its handle's read first. */
void CloseTextFont(struct TextFont *tf)
{
    Handle h = tf && (tf->tf_Flags & FPF_OUTLINE) && tf->tf_Cache ? tf->tf_Cache->area : 0;

    if (h)
        FreeArea(h);
}

void SetFont(struct RastPort *rp, const struct TextFont *tf)
{
    if (rp)
        rp->rp_Font = tf;
}

static void empty(struct GfxFontCache *c)
{
    memset(c->slots, 0, SLOTS * sizeof(struct cached));
    c->used = 0;
    c->arena_used = 0;
}

/* A glyph at a phase, from the cache, or drawn into it. */
static const struct cached *glyph(const struct gfx_rows *k, struct GfxFontCache *c, uint32_t g,
                                  uint32_t phase)
{
    uint32_t key = g * 4 + phase + 1, i;
    struct GfxPath path = { c->points, c->verbs, MAX_POINTS, MAX_POINTS, 0, 0 };
    struct cached *e;
    struct GfxRect b;
    int64_t x0, y0, x1, y1;

    for (i = (key * 2654435761u) >> 22;; i = (i + 1) & (SLOTS - 1)) {
        if (c->slots[i].key == key)
            return &c->slots[i];
        if (!c->slots[i].key)
            break;
    }
    if (c->used >= SLOTS_FULL || c->arena_size - c->arena_used < (uint64_t)c->bw * c->bh) {
        empty(c);
        for (i = (key * 2654435761u) >> 22; c->slots[i].key; i = (i + 1) & (SLOTS - 1))
            ;
    }
    e = &c->slots[i];
    *e = (struct cached){ .key = key };
    c->used++;
    if (c->ifont->GlyphOutline(&c->face, g, c->tf.tf_Size, &path) != 0)
        return e;
    for (uint32_t p = 0; p < path.gp_NumPoints; p++)
        path.gp_Points[p].x += (int32_t)(phase * 64 / c->phases);
    if (!PathBounds(&path, &b))
        return e;
    x0 = b.MinX > c->bx ? b.MinX : c->bx;
    y0 = b.MinY > c->by ? b.MinY : c->by;
    x1 = b.MaxX < c->bx + (int64_t)c->bw - 1 ? b.MaxX : c->bx + (int64_t)c->bw - 1;
    y1 = b.MaxY < c->by + (int64_t)c->bh - 1 ? b.MaxY : c->by + (int64_t)c->bh - 1;
    if (x0 > x1 || y0 > y1)
        return e;
    struct GfxMask m = {
        .gm_Pixels = c->arena + c->arena_used, .gm_Width = (uint32_t)(x1 - x0 + 1),
        .gm_Height = (uint32_t)(y1 - y0 + 1), .gm_BytesPerRow = (uint32_t)(x1 - x0 + 1),
        .gm_X = (int32_t)x0, .gm_Y = (int32_t)y0,
    };
    if (!gfx_path_mask(k, &path, 0, &m, c->scratch, c->scratch_size))
        return e;
    *e = (struct cached){ key, (int16_t)x0, (int16_t)y0, (uint16_t)m.gm_Width, (uint16_t)m.gm_Height,
                          (uint32_t)c->arena_used };
    c->arena_used += (uint64_t)m.gm_Width * m.gm_Height;
    return e;
}

/*
 * n glyphs drawn in the foreground pen, the pen 'pen' (26.6 px) on the
 * baseline y; the pen after them back. Each glyph lands where the pen
 * and its offset say, in font units summed and then scaled, rounded to
 * the nearest phase.
 */
static int64_t draw(const struct gfx_rows *k, struct RastPort *rp, struct GfxFontCache *c,
                    const struct ShapedGlyph *gl, uint32_t n, int64_t pen, int32_t y)
{
    const struct FontIFace *f = c->ifont;
    int64_t units = 0;

    for (uint32_t i = 0; i < n; i++) {
        const struct ShapedGlyph *s = &gl[i];
        int64_t x26 = pen + f->ScaleUnits(&c->face, (int32_t)(units + s->sg_XOffset), c->tf.tf_Size);
        int64_t q = floor_div(x26 * (int64_t)c->phases + 32, 64);     /* in phases */
        int64_t dy = floor_div(-(int64_t)f->ScaleUnits(&c->face, s->sg_YOffset, c->tf.tf_Size) + 32, 64);
        uint32_t phase = (uint32_t)(q - floor_div(q, c->phases) * c->phases);
        const struct cached *e;

        units += s->sg_Advance;
        if (s->sg_Flags & GLYPHF_HIDDEN)
            continue;
        e = glyph(k, c, s->sg_Glyph, phase);
        if (!e->w)
            continue;
        struct GfxMask m = { c->arena + e->at, e->w, e->h, e->w, e->x, e->y };
        int64_t gx = floor_div(q, c->phases), gy = (int64_t)y + dy;

        if (gx >= INT32_MIN && gx <= INT32_MAX && gy >= INT32_MIN && gy <= INT32_MAX)
            gfx_blt_mask(k, rp, &m, (int32_t)gx, (int32_t)gy);
    }
    return pen + f->ScaleUnits(&c->face, (int32_t)units, c->tf.tf_Size);
}

void gfx_text_run(const struct gfx_rows *k, struct RastPort *rp, const struct GlyphRun *run,
                  uint32_t first, uint32_t count, int32_t x, int32_t y)
{
    struct GfxFontCache *c = (struct GfxFontCache *)outline_of(rp);

    if (!c || !run || !run->gr_Glyphs || first >= run->gr_Count)
        return;
    count = count < run->gr_Count - first ? count : run->gr_Count - first;
    draw(k, rp, c, run->gr_Glyphs + first, count, (int64_t)x * 64, y);
}

/* The next piece of Text's string: at most RUN_GLYPHS bytes, cut
 * before a character's first byte. */
static uint64_t piece(const char *s, uint64_t n)
{
    uint64_t cut = n < RUN_GLYPHS ? n : RUN_GLYPHS;

    if (cut < n)
        for (unsigned back = 0; back < 3 && cut > 1 && ((uint8_t)s[cut] & 0xc0) == 0x80; back++)
            cut--;
    return cut;
}

/* Shapes a piece into the font's glyphs: false if it won't shape. */
static bool shape(struct GfxFontCache *c, const char *s, uint64_t n, struct GlyphRun *run)
{
    *run = (struct GlyphRun){ .gr_Glyphs = c->glyphs, .gr_Max = RUN_GLYPHS };
    return c->ifont->ShapeRun(&c->face, s, n, c->features, run) == 0;
}

void gfx_text(const struct gfx_rows *k, struct RastPort *rp, int32_t x, int32_t y, const char *string,
              uint64_t count)
{
    struct GfxFontCache *c = (struct GfxFontCache *)outline_of(rp);
    int64_t pen = (int64_t)x * 64;
    struct GlyphRun run;

    if (!c) {
        gfx_text_bitmap(rp, x, y, string, count);
        return;
    }
    if (!string)
        return;
    while (count) {
        uint64_t n = piece(string, count);

        if (!shape(c, string, n, &run))
            return;
        if (rp->rp_DrawMode == JAM2) {
            int64_t end = pen + c->ifont->ScaleUnits(&c->face, run.gr_Advance, c->tf.tf_Size);
            int64_t x0 = floor_div(pen + 32, 64), x1 = floor_div(end + 32, 64) - 1;

            if (x1 >= x0 && x0 >= INT32_MIN && x1 <= INT32_MAX)
                gfx_rect_fill_pen(k, rp, x0, y, x1, (int64_t)y + c->tf.tf_YSize - 1, rp->rp_BgPen);
        }
        pen = draw(k, rp, c, run.gr_Glyphs, run.gr_Count, pen, y + c->tf.tf_Baseline);
        string += n;
        count -= n;
    }
}

int32_t TextLength(struct RastPort *rp, const char *string, uint64_t count)
{
    const struct GfxFontCache *cc = outline_of(rp);
    struct GfxFontCache *c = (struct GfxFontCache *)cc;
    int64_t width = 0;
    struct GlyphRun run;

    if (!rp || !string)
        return 0;
    if (!c) {
        const struct TextFont *tf = rp->rp_Font;
        uint64_t w = (uint64_t)(tf ? tf->tf_XSize : 8) * count;

        return w > INT32_MAX ? INT32_MAX : (int32_t)w;
    }
    while (count) {
        uint64_t n = piece(string, count);

        if (!shape(c, string, n, &run))
            break;
        width += c->ifont->ScaleUnits(&c->face, run.gr_Advance, c->tf.tf_Size);
        string += n;
        count -= n;
    }
    width = floor_div(width + 32, 64);
    return width > INT32_MAX ? INT32_MAX : width < INT32_MIN ? INT32_MIN : (int32_t)width;
}

void TextRun(struct RastPort *rp, const struct GlyphRun *run, uint32_t first, uint32_t count, int32_t x,
             int32_t y)
{
    gfx_text_run(&gfx_rows_scalar, rp, run, first, count, x, y);
}

void Text(struct RastPort *rp, int32_t x, int32_t y, const char *string, uint64_t count)
{
    gfx_text(&gfx_rows_scalar, rp, x, y, string, count);
}
