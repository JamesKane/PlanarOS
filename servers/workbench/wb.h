/*
 * servers/workbench/wb.h -- what the Workbench draws, for the Workbench
 * and for the tests that hold it to what they draw themselves
 * (conformance/window.c on QEMU, servers/workbench/test.c on the host)
 * (V8 step 7)
 *
 * The backdrop (§13.2): bg.backdrop.base's gradient, top to bottom, and
 * over it three curved planes, as paths, each a step lighter than the
 * gradient where it is (bg.backdrop.plane's, with §13.2's colours). The
 * volumes' icons (§13.4, decision 3: drawn in code, 64 px): System, the
 * volume SYS: is on, a two-tier cobalt slab with a gear; Storage, any
 * other, an amber and crimson stack with an activity light. They stand in
 * a column on the left, 24 px in, 88 px apart (§13.5), each labelled
 * below in System Sans; one selected has its lid lifted, its rim or light
 * brighter, and its label on a cobalt pill.
 *
 * A volume's or a drawer's window (wb_drawer_build): a ui.library list of
 * its entries, a drawer's name with a '/' after it, and below it how many
 * of each there are.
 */
#ifndef SERVERS_WORKBENCH_WB_H
#define SERVERS_WORKBENCH_WB_H

#include <stdint.h>

#include <planar/dos.h>
#include <planar/gfx.h>
#include <planar/intuition.h>
#include <planar/ui.h>

constexpr unsigned WB_VOLUMES = 8;          /* dos's, at most */
constexpr unsigned WB_NAME = 32;            /* a volume's name, its end included */
constexpr int32_t WB_ICON = 64;             /* an icon's canvas, px */
constexpr int32_t WB_MARGIN = 24, WB_TOP = 16, WB_PITCH = 88;
constexpr int32_t WB_LABEL_Y = WB_ICON + 2;    /* a label's line's top, from its icon's */
constexpr int32_t WB_PILL_PAD = 6;
constexpr uint64_t WB_DOUBLE_NS = 500000000;   /* a double click's second click, within */

/* A volume's or drawer's window: where the first goes, its inside's size,
 * and the step to the next. */
constexpr int32_t WB_DRAWER_LEFT = 140, WB_DRAWER_TOP = 60, WB_DRAWER_STEP = 30;
constexpr uint32_t WB_DRAWER_W = 320, WB_DRAWER_H = 240;
constexpr unsigned WB_ENTRIES = 64;         /* a window's, at most: the rest aren't shown */
constexpr unsigned WB_ENTRY = 40;           /* an entry's label, its end included */

enum : uint8_t { WB_STORAGE = 0, WB_SYSTEM = 1 };

struct wb_icon {
    char name[WB_NAME];                 /* dos's: "PLANAR" */
    uint8_t kind;
    bool selected;
    int32_t x, y;                       /* its canvas's top left, in the backdrop */
};

/* Scratch for a path. */
struct wb_path {
    struct GfxPoint p[48];
    uint8_t v[24];
    struct GfxPath gp;
};

static inline struct GfxPath *wb_path_new(struct wb_path *s)
{
    s->gp = (struct GfxPath){ s->p, s->v, sizeof s->p / sizeof s->p[0], sizeof s->v, 0, 0 };
    return &s->gp;
}

static inline void wb_add(struct wb_path *s, uint8_t verb, int32_t x, int32_t y)
{
    if (s->gp.gp_NumVerbs < s->gp.gp_MaxVerbs)
        s->v[s->gp.gp_NumVerbs++] = verb;
    if (verb != PATH_CLOSE && s->gp.gp_NumPoints < s->gp.gp_MaxPoints)
        s->p[s->gp.gp_NumPoints++] = (struct GfxPoint){ x, y };
}

/* A polygon of n points, (x0 + pts[2i], y0 + pts[2i + 1]) in px, filled. */
static inline void wb_poly(const struct GfxIFace *g, struct RastPort *rp, int32_t x0, int32_t y0, const int8_t *pts,
                           unsigned n, uint32_t pen)
{
    struct wb_path s;

    wb_path_new(&s);
    for (unsigned i = 0; i < n; i++)
        wb_add(&s, i ? PATH_LINE : PATH_MOVE, (x0 + pts[2 * i]) * 64, (y0 + pts[2 * i + 1]) * 64);
    wb_add(&s, PATH_CLOSE, 0, 0);
    rp->rp_FgPen = pen;
    g->FillPath(rp, &s.gp, 0);
}

/* The same, filled with a gradient top to bottom over its box. */
static inline void wb_poly_grad(const struct GfxIFace *g, struct RastPort *rp, int32_t x0, int32_t y0,
                                const int8_t *pts, unsigned n, uint32_t from, uint32_t to)
{
    struct wb_path s;
    struct GfxGradient gg;
    int32_t top = 127, bottom = -128;

    wb_path_new(&s);
    for (unsigned i = 0; i < n; i++) {
        wb_add(&s, i ? PATH_LINE : PATH_MOVE, (x0 + pts[2 * i]) * 64, (y0 + pts[2 * i + 1]) * 64);
        top = pts[2 * i + 1] < top ? pts[2 * i + 1] : top;
        bottom = pts[2 * i + 1] > bottom ? pts[2 * i + 1] : bottom;
    }
    wb_add(&s, PATH_CLOSE, 0, 0);
    gg = (struct GfxGradient){ { x0, y0 + top, x0 + WB_ICON - 1, y0 + bottom }, from, to, 0 };
    g->FillGradient(rp, &gg, &s.gp, 0);
}

/* 'c' with 'd' added to each channel, held to 255. */
static inline uint32_t wb_lighter(uint32_t c, uint32_t d)
{
    uint32_t out = 0xff000000u;

    for (unsigned k = 0; k < 24; k += 8) {
        uint32_t v = (c >> k & 0xff) + (d >> k & 0xff);

        out |= (v > 255 ? 255 : v) << k;
    }
    return out;
}

/* --- The backdrop. --- */

/* w by h of the backdrop in rp at (0, 0): the gradient, then the planes. */
static inline void wb_backdrop(const struct GfxIFace *g, struct RastPort *rp, uint32_t w, uint32_t h, uint32_t top,
                               uint32_t bottom)
{
    struct GfxGradient gg = { { 0, 0, (int32_t)w - 1, (int32_t)h - 1 }, 0xff000000u | top, 0xff000000u | bottom, 0 };
    int64_t W = (int64_t)w * 64, H = (int64_t)h * 64;
    struct wb_path s;

    g->FillGradient(rp, &gg, nullptr, 0);

    /* Low across the whole width, rising to the right. */
    wb_path_new(&s);
    wb_add(&s, PATH_MOVE, 0, (int32_t)(H * 62 / 100));
    s.v[s.gp.gp_NumVerbs++] = PATH_CUBIC;
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)(W * 30 / 100), (int32_t)(H * 48 / 100) };
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)(W * 62 / 100), (int32_t)(H * 82 / 100) };
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)W, (int32_t)(H * 58 / 100) };
    wb_add(&s, PATH_LINE, (int32_t)W, (int32_t)H);
    wb_add(&s, PATH_LINE, 0, (int32_t)H);
    wb_add(&s, PATH_CLOSE, 0, 0);
    rp->rp_FgPen = wb_lighter(top, 0x0a0d12);
    g->FillPath(rp, &s.gp, 0);

    /* A sweep from the bottom's middle up to the right edge. */
    wb_path_new(&s);
    wb_add(&s, PATH_MOVE, (int32_t)(W * 34 / 100), (int32_t)H);
    s.v[s.gp.gp_NumVerbs++] = PATH_CUBIC;
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)(W * 50 / 100), (int32_t)(H * 70 / 100) };
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)(W * 76 / 100), (int32_t)(H * 52 / 100) };
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)W, (int32_t)(H * 36 / 100) };
    wb_add(&s, PATH_LINE, (int32_t)W, (int32_t)H);
    wb_add(&s, PATH_CLOSE, 0, 0);
    rp->rp_FgPen = wb_lighter(bottom, 0x0a0710);
    g->FillPath(rp, &s.gp, 0);

    /* A shallow arc across the top right. */
    wb_path_new(&s);
    wb_add(&s, PATH_MOVE, (int32_t)(W * 52 / 100), 0);
    s.v[s.gp.gp_NumVerbs++] = PATH_CUBIC;
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)(W * 66 / 100), (int32_t)(H * 16 / 100) };
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)(W * 84 / 100), (int32_t)(H * 22 / 100) };
    s.p[s.gp.gp_NumPoints++] = (struct GfxPoint){ (int32_t)W, (int32_t)(H * 14 / 100) };
    wb_add(&s, PATH_LINE, (int32_t)W, 0);
    wb_add(&s, PATH_CLOSE, 0, 0);
    rp->rp_FgPen = wb_lighter(top, 0x0a0d12);
    g->FillPath(rp, &s.gp, 0);
}

/* --- Icons. --- */

/* Where each icon goes: down the left, in dos's order. */
static inline void wb_place(struct wb_icon *icons, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        icons[i].x = WB_MARGIN;
        icons[i].y = WB_TOP + (int32_t)i * WB_PITCH;
    }
}

static inline uint64_t wb_len(const char *s)
{
    uint64_t n = 0;

    while (s[n])
        n++;
    return n;
}

/* An icon's rectangle in the backdrop: its canvas and its label's pill. */
static inline struct GfxRect wb_icon_rect(const struct GfxIFace *g, struct RastPort *rp, const struct TextFont *font,
                                          const struct wb_icon *ic)
{
    int32_t tw, mid = ic->x + WB_ICON / 2, half;

    g->SetFont(rp, font);
    tw = g->TextLength(rp, ic->name, wb_len(ic->name));
    half = tw / 2 + WB_PILL_PAD;
    half = half < WB_ICON / 2 ? WB_ICON / 2 : half;
    return (struct GfxRect){ mid - half, ic->y, mid + half - 1, ic->y + WB_LABEL_Y + font->tf_YSize + 4 };
}

/* An isometric box on the canvas at (x, y): its top face's corners at
 * (l, mid), (32, back), (64 - l, mid) and (32, front), its sides 'depth'
 * px deep; the top filled from top_from to top_to, the sides flat. */
static inline void wb_box(const struct GfxIFace *g, struct RastPort *rp, int32_t x, int32_t y, int8_t l, int8_t back,
                          int8_t mid, int8_t front, int8_t depth, uint32_t top_from, uint32_t top_to, uint32_t left,
                          uint32_t right)
{
    const int8_t lf[8] = { l, mid, 32, front, 32, (int8_t)(front + depth), l, (int8_t)(mid + depth) };
    const int8_t rf[8] = { 32, front, (int8_t)(64 - l), mid, (int8_t)(64 - l), (int8_t)(mid + depth), 32,
                           (int8_t)(front + depth) };
    const int8_t tf[8] = { l, mid, 32, back, (int8_t)(64 - l), mid, 32, front };

    wb_poly(g, rp, x, y, lf, 4, left);
    wb_poly(g, rp, x, y, rf, 4, right);
    wb_poly_grad(g, rp, x, y, tf, 4, top_from, top_to);
}

static const int8_t wb_shadow[8] = { 8, 46, 34, 33, 60, 46, 34, 59 };

/* System: two cobalt plates, a cyan rim between, a gear on top. */
static inline void wb_icon_system(const struct GfxIFace *g, struct RastPort *rp, int32_t x, int32_t y, bool sel)
{
    /* cos and sin of 0, 30, ... 330 degrees, in 16ths. */
    static const int8_t co[12] = { 16, 14, 8, 0, -8, -14, -16, -14, -8, 0, 8, 14 };
    static const int8_t si[12] = { 0, 8, 14, 16, 14, 8, 0, -8, -14, -16, -14, -8 };
    static const int8_t rim[12] = { 12, 33, 32, 43, 52, 33, 52, 35, 32, 45, 12, 35 };
    static const int8_t hub[8] = { 29, 24, 32, 22, 35, 24, 32, 26 };
    int32_t up = y - (sel ? 3 : 0);
    int8_t gear[24];

    wb_poly(g, rp, x, y, wb_shadow, 4, 0xff0e1424);
    wb_box(g, rp, x, y, 6, 22, 35, 48, 10, 0xff38bdf8, 0xff1d4ed8, 0xff1e40af, 0xff172f78);
    wb_poly(g, rp, x, up, rim, 6, sel ? 0xffa5f3fc : 0xff38bdf8);
    wb_box(g, rp, x, up, 12, 14, 24, 34, 9, 0xff60a5fa, 0xff2563eb, 0xff1d4ed8, 0xff1e3a8a);
    /* Six teeth, on an ellipse the projection squashes to half its height. */
    for (unsigned i = 0; i < 12; i++) {
        int32_t r = i % 2 ? 7 : 10;

        gear[2 * i] = (int8_t)(32 + co[i] * r / 16);
        gear[2 * i + 1] = (int8_t)(24 + si[i] * r / 32);
    }
    wb_poly(g, rp, x, up, gear, 12, 0xffcfd8e6);
    wb_poly(g, rp, x, up, hub, 4, 0xff1e3a8a);
}

/* Storage: an amber and crimson stack, a data plate on top, a red light
 * along its front bevel. */
static inline void wb_icon_storage(const struct GfxIFace *g, struct RastPort *rp, int32_t x, int32_t y, bool sel)
{
    static const int8_t plate[8] = { 20, 26, 32, 20, 44, 26, 32, 32 };
    static const int8_t light[8] = { 38, 45, 50, 39, 50, 41, 38, 47 };
    int32_t up = y - (sel ? 3 : 0);

    wb_poly(g, rp, x, y, wb_shadow, 4, 0xff0e1424);
    wb_box(g, rp, x, y, 8, 30, 40, 50, 8, 0xffea580c, 0xff9f1d1d, 0xff7c2d12, 0xff6b1414);
    wb_box(g, rp, x, up, 8, 14, 26, 38, 12, 0xfff97316, 0xffb91c1c, 0xff9a3412, 0xff7f1d1d);
    wb_poly(g, rp, x, up, plate, 4, 0xffc2410c);
    wb_poly(g, rp, x, up, light, 4, sel ? 0xfffca5a5 : 0xffef4444);
}

/* An icon and its label at ic->x, ic->y. */
static inline void wb_icon_draw(const struct GfxIFace *g, struct RastPort *rp, const struct TextFont *font,
                                const struct wb_icon *ic)
{
    struct GfxRect r = wb_icon_rect(g, rp, font, ic);
    uint64_t n = wb_len(ic->name);
    int32_t tw, ty = ic->y + WB_LABEL_Y;

    if (ic->kind == WB_SYSTEM)
        wb_icon_system(g, rp, ic->x, ic->y, ic->selected);
    else
        wb_icon_storage(g, rp, ic->x, ic->y, ic->selected);
    g->SetFont(rp, font);
    tw = g->TextLength(rp, ic->name, n);
    if (ic->selected) {
        struct wb_path s;
        struct GfxRect pill = { r.MinX, ty, r.MaxX, ty + font->tf_YSize + 3 };

        g->PathRoundRect(wb_path_new(&s), &pill, 8 * 64);
        rp->rp_FgPen = 0xff2348a6;      /* #2563EB at 60% over the backdrop */
        g->FillPath(rp, &s.gp, 0);
    }
    rp->rp_DrawMode = JAM1;
    rp->rp_FgPen = 0xff0a0e18;
    g->Text(rp, ic->x + WB_ICON / 2 - tw / 2, ty + 2 + 1, ic->name, n);
    rp->rp_FgPen = 0xfff0f4fa;
    g->Text(rp, ic->x + WB_ICON / 2 - tw / 2, ty + 2, ic->name, n);
}

/* --- The volumes, from dos. --- */

/* dos's volumes, in its order: System the first holding EFI/planar (as
 * dos takes SYS:), Storage the rest. How many. */
static inline unsigned wb_volumes(struct Dos *dos, struct wb_icon *icons, unsigned max)
{
    struct DosEntry e;
    unsigned n = 0;
    bool sys = false;

    while (n < max && NextVolume(dos, n, &e) == 0) {
        char path[WB_NAME + 16];
        unsigned k = 0;

        for (; e.de_Name[k] && k < WB_NAME - 1; k++)
            icons[n].name[k] = e.de_Name[k];
        icons[n].name[k] = 0;
        SNPrintf(path, sizeof path, "%s:EFI/planar", icons[n].name);
        icons[n].kind = !sys && ExaminePath(dos, path, &e) == 0 && e.de_Dir ? WB_SYSTEM : WB_STORAGE;
        sys |= icons[n].kind == WB_SYSTEM;
        icons[n].selected = false;
        n++;
    }
    wb_place(icons, n);
    return n;
}

/* --- A drawer's window. --- */

enum : uint32_t { WB_D_ROOT = 1, WB_D_LIST, WB_D_STATUS };

struct wb_drawer {
    char label[WB_ENTRIES][WB_ENTRY];
    const char *items[WB_ENTRIES];
    bool dir[WB_ENTRIES];
    unsigned count;
    unsigned files, dirs;               /* all of them, shown or not */
    char status[48];
};

/* An entry, as its row says it: a drawer's name with a '/'. */
static inline void wb_entry(struct wb_drawer *d, const struct DosEntry *e)
{
    unsigned k = 0, i = d->count;

    if (e->de_Dir)
        d->dirs++;
    else
        d->files++;
    if (i == WB_ENTRIES)
        return;
    for (; e->de_Name[k] && k < WB_ENTRY - 2; k++)
        d->label[i][k] = e->de_Name[k];
    if (e->de_Dir)
        d->label[i][k++] = '/';
    d->label[i][k] = 0;
    d->items[i] = d->label[i];
    d->dir[i] = e->de_Dir;
    d->count++;
}

/* A directory's entries, in dos's order: 0, or why not. */
static inline int64_t wb_read(struct Dos *dos, const char *path, struct wb_drawer *d)
{
    struct DosEntry e;
    uint32_t cursor = 0;
    int64_t f;

    d->count = d->files = d->dirs = 0;
    if ((f = OpenFile(dos, path, 0, &e)) < 0)
        return f;
    if (!e.de_Dir) {
        CloseFile(dos, (uint32_t)f);
        return ERR_BADARG;
    }
    while (ExamineNext(dos, (uint32_t)f, &cursor, &e) == 0)
        wb_entry(d, &e);
    CloseFile(dos, (uint32_t)f);
    SNPrintf(d->status, sizeof d->status, "%u drawer%s, %u file%s", d->dirs, d->dirs == 1 ? "" : "s", d->files,
             d->files == 1 ? "" : "s");
    return 0;
}

/* The window's gadgets: the list, and the line below it. */
static inline bool wb_drawer_build(const struct UIIFace *iui, struct UI *ui, const struct wb_drawer *d)
{
    uint32_t n = 0;

    n += AddGadgetTags(iui, ui, 0, UI_COLUMN, { UIA_Margin, 8 }, { UIA_Spacing, 6 }) == WB_D_ROOT;
    n += AddGadgetTags(iui, ui, WB_D_ROOT, UI_LIST, { UIA_Items, (uint64_t)d->items }, { UIA_Count, d->count },
                       { UIA_Value, (uint64_t)-1 }, { UIA_Weight, 1 }) == WB_D_LIST;
    n += AddGadgetTags(iui, ui, WB_D_ROOT, UI_LABEL, { UIA_Label, (uint64_t)d->status }) == WB_D_STATUS;
    return n == WB_D_STATUS;
}

/* The k-th drawer window's place, cascaded. */
static inline void wb_drawer_at(unsigned k, int32_t *left, int32_t *top)
{
    *left = WB_DRAWER_LEFT + WB_DRAWER_STEP * (int32_t)(k % 8);
    *top = WB_DRAWER_TOP + WB_DRAWER_STEP * (int32_t)(k % 8);
}

#endif /* SERVERS_WORKBENCH_WB_H */
