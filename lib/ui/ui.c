/*
 * lib/ui/ui.c -- ui.library: gadgets in the caller's window (V8 step 6)
 *
 * §13.3's six gadgets and decision 8's four, retained in the caller's
 * struct UI and drawn by the caller's gfx.library, in §13.2's colours:
 * a pane's gradient under everything, labels light with a shadow on a
 * dark pane and dark with an etch on a light one, cobalt and amber for
 * what's on or chosen, thumbs in silver, a focused gadget ringed in
 * amber. Shapes are gfx's rounded rectangles.
 *
 * Every gadget is drawn on its pane's gradient, clipped to its own
 * rectangle, so one drawn again alone (an event's) gives the pixels it
 * has when the whole inside is drawn: the host test holds the two to
 * each other. Gadgets don't overlap, and nothing's drawn outside one's
 * rectangle: a focusable gadget keeps FOCUS px around it for its ring.
 *
 * As a shared library it keeps no data: everything's in the UI.
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/ui.h>

void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);

/* Sizes (§13.3), px. */
constexpr int32_t FOCUS = 2, GAP = 12, GROOVE = 2;
constexpr int32_t TOGGLE_W = 52, TOGGLE_H = 22, THUMB_W = 24, THUMB_H = 18;
constexpr int32_t SLIDER_W = 140, KNOB_W = 14, KNOB_H = 18, TRACK_H = 6;
constexpr int32_t SEG_H = 26, SEG_PAD = 24, BUTTON_H = 26, BUTTON_W = 80, BUTTON_PAD = 32;
constexpr int32_t ROW_H = 22, LIST_W = 160, LIST_ROWS = 4, FIELD_W = 160, FIELD_H = 24, FIELD_PAD = 6;
constexpr int32_t BADGE_PADX = 10, BADGE_PADY = 6;

/* Keys' usages (HID's), as IDCMP_RAWKEY has them. */
enum : uint16_t { KEY_HOME = 0x4a, KEY_END = 0x4d, KEY_RIGHT = 0x4f, KEY_LEFT = 0x50, KEY_DOWN = 0x51, KEY_UP = 0x52 };

/* §13.2's tokens: a pane's, dark and light, then the controls'. */
struct pane {
    uint32_t top, bottom;               /* pane.dark.bg, pane.light.bg */
    uint32_t text, under;               /* a label, and its shadow or etch a pixel below */
    uint32_t groove_dark, groove_light; /* a divider's two rows */
    uint32_t well, well_edge, well_text;    /* a list's and a text field's */
};

static const struct pane panes[2] = {
    { 0xff36435a, 0xff46556e, 0xfff0f4fa, 0xff0e131d, 0xff242e40, 0xff50607a, 0xff253144, 0xff1a2434, 0xfff0f4fa },
    { 0xffd8dee8, 0xffb8c2d1, 0xff141824, 0xffe9eef5, 0xff8e9ab0, 0xfff4f7fb, 0xfff4f7fb, 0xff6e7b90, 0xff1a2230 },
};

constexpr uint32_t TEXT_MUTED = 0xff7e8ca3, TEXT_DARK = 0xff141824, TEXT_LIGHT = 0xfff0f4fa;
constexpr uint32_t AMBER_TOP = 0xffeb861e, AMBER_BOTTOM = 0xffd46b0e, FOCUS_RING = 0xfff08c20;
constexpr uint32_t COBALT = 0xff2e7cf6, COBALT_TOP = 0xff3b82f6, COBALT_BOTTOM = 0xff2563eb;
constexpr uint32_t TRACK_OFF = 0xff253144, AMBER_OFF = 0xff58657a, TRACK_DISABLED = 0xff5a6578;
constexpr uint32_t THUMB_TOP = 0xfff4f7fb, THUMB_BOTTOM = 0xffcfd8e6, THUMB_EDGE = 0xff6e7b90;
constexpr uint32_t THUMB_DIS_TOP = 0xff8a95a8, THUMB_DIS_BOTTOM = 0xff6f7a8d;
constexpr uint32_t SLIDER_TRACK = 0xff232f42, SLIDER_LIGHT = 0xff566884, SLIDER_FILL = 0xff3b82f6;
constexpr uint32_t KNOB = 0xfff0f4fa, KNOB_EDGE = 0xff1a2434, FILL_DISABLED = 0xff526078;
constexpr uint32_t SEG_TOP = 0xffdce3ef, SEG_BOTTOM = 0xffc8d0dd, SEG_EDGE = 0xff6e7b90, SEG_LINE = 0xffa8b4c6;
constexpr uint32_t SPLIT_DARK = 0xff1e2736, SPLIT_LIGHT = 0xffe8eef7;
constexpr uint32_t BADGE = 0xff232c40, BADGE_EDGE = 0xff566480;

static struct UIGadget *G(struct UI *ui, uint32_t n)
{
    return &ui->ui_Gadget[n];
}

static bool group(uint8_t kind)
{
    return kind == UI_ROW || kind == UI_COLUMN || kind == UI_SPLITPANE;
}

static bool focusable(uint8_t kind)
{
    return kind == UI_TOGGLE || kind == UI_SLIDER || kind == UI_SEGMENTED || kind == UI_BUTTON ||
           kind == UI_LIST || kind == UI_TEXTFIELD;
}

static bool within(struct GfxRect r, int32_t x, int32_t y)
{
    return x >= r.MinX && x <= r.MaxX && y >= r.MinY && y <= r.MaxY;
}

static struct GfxRect inset(struct GfxRect r, int32_t d)
{
    return (struct GfxRect){ r.MinX + d, r.MinY + d, r.MaxX - d, r.MaxY - d };
}

static struct GfxRect meet(struct GfxRect a, struct GfxRect b)
{
    return (struct GfxRect){ a.MinX > b.MinX ? a.MinX : b.MinX, a.MinY > b.MinY ? a.MinY : b.MinY,
                             a.MaxX < b.MaxX ? a.MaxX : b.MaxX, a.MaxY < b.MaxY ? a.MaxY : b.MaxY };
}

static uint64_t length(const char *s)
{
    uint64_t n = 0;

    while (s && s[n])
        n++;
    return n;
}

static int32_t line_h(const struct TextFont *f)
{
    return f ? f->tf_YSize : 8;
}

static int32_t width(struct UI *ui, const struct TextFont *f, const char *s, uint64_t n)
{
    struct RastPort rp = { .rp_Font = f };

    return s && n ? ui->ui_Gfx->TextLength(&rp, s, n) : 0;
}

/* --- Drawing. --- */

static struct GfxPath new_path(struct UI *ui)
{
    return (struct GfxPath){ ui->ui_Points, ui->ui_Verbs, sizeof ui->ui_Points / sizeof ui->ui_Points[0],
                             sizeof ui->ui_Verbs, 0, 0 };
}

static void rect(struct UI *ui, uint32_t pen, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    ui->ui_RPort->rp_FgPen = pen;
    ui->ui_Gfx->RectFill(ui->ui_RPort, x0, y0, x1, y1);
}

/* A rounded rectangle filled from 'from' at grad's top to 'to' at its
 * bottom, its outline 'edge' a pixel wide (none if 0). */
static void shape(struct UI *ui, struct GfxRect r, int32_t radius, uint32_t from, uint32_t to, uint32_t edge,
                  struct GfxRect grad)
{
    const struct GfxIFace *g = ui->ui_Gfx;
    struct GfxPath p = new_path(ui);
    struct GfxGradient gg = { grad, from, to, 0 };

    if (edge) {
        g->PathRoundRect(&p, &r, radius * 64);
        ui->ui_RPort->rp_FgPen = edge;
        g->FillPath(ui->ui_RPort, &p, 0);
        p = new_path(ui);
        r = inset(r, 1);
        radius = radius > 1 ? radius - 1 : 0;
    }
    g->PathRoundRect(&p, &r, radius * 64);
    g->FillGradient(ui->ui_RPort, &gg, &p, 0);
}

static void rounded(struct UI *ui, struct GfxRect r, int32_t radius, uint32_t from, uint32_t to, uint32_t edge)
{
    shape(ui, r, radius, from, to, edge, r);
}

/* The focus ring: a pixel of amber, 'd' outside r. */
static void ring(struct UI *ui, struct GfxRect r, int32_t radius, int32_t d)
{
    struct GfxPath p = new_path(ui);
    struct GfxRect out = inset(r, -d), in = inset(out, 1);

    ui->ui_Gfx->PathRoundRect(&p, &out, (radius + d) * 64);
    ui->ui_Gfx->PathRoundRect(&p, &in, (radius + d - 1) * 64);
    ui->ui_RPort->rp_FgPen = FOCUS_RING;
    ui->ui_Gfx->FillPath(ui->ui_RPort, &p, PATHF_EVENODD);
}

/* Text at (x, y), its line's top: 'under' a pixel below it first (0: none). */
static void text(struct UI *ui, const struct TextFont *f, uint32_t pen, uint32_t under, int32_t x, int32_t y,
                 const char *s, uint64_t n)
{
    struct RastPort *rp = ui->ui_RPort;

    if (!s || !n)
        return;
    ui->ui_Gfx->SetFont(rp, f);
    rp->rp_DrawMode = JAM1;
    if (under) {
        rp->rp_FgPen = under;
        ui->ui_Gfx->Text(rp, x, y + 1, s, n);
    }
    rp->rp_FgPen = pen;
    ui->ui_Gfx->Text(rp, x, y, s, n);
}

/* The pane a gadget's on: its own, or its nearest group's that has one. */
static uint32_t pane_of(struct UI *ui, uint32_t n)
{
    while (n && !G(ui, n)->g_Pane)
        n = G(ui, n)->g_Parent;
    return n;
}

static const struct pane *colours(struct UI *ui, uint32_t n)
{
    uint32_t p = pane_of(ui, n);

    return &panes[p && G(ui, p)->g_Pane == UIPANE_DARK ? 0 : 1];
}

static void pane_fill(struct UI *ui, uint32_t p)
{
    struct GfxRect r = p ? G(ui, p)->g_Rect
                         : (struct GfxRect){ 0, 0, (int32_t)ui->ui_Width - 1, (int32_t)ui->ui_Height - 1 };
    const struct pane *c = &panes[p && G(ui, p)->g_Pane == UIPANE_DARK ? 0 : 1];
    struct GfxGradient gg = { r, c->top, c->bottom, 0 };

    ui->ui_Gfx->FillGradient(ui->ui_RPort, &gg, nullptr, 0);
}

/* A gadget's label at its left, upright in its rectangle. */
static void label(struct UI *ui, uint32_t n, const struct UIGadget *g)
{
    const struct pane *c = colours(ui, n);
    int32_t y = g->g_Rect.MinY + (g->g_Rect.MaxY - g->g_Rect.MinY + 1 - line_h(ui->ui_Sans)) / 2;

    text(ui, ui->ui_Sans, g->g_Disabled ? TEXT_MUTED : c->text, g->g_Disabled ? 0 : c->under, g->g_Rect.MinX, y,
         g->g_Label, length(g->g_Label));
}

static int32_t label_w(struct UI *ui, const struct UIGadget *g)
{
    return g->g_Label ? width(ui, ui->ui_Sans, g->g_Label, length(g->g_Label)) + GAP : 0;
}

/* Where a control's drawn in its gadget's rectangle. */
static struct GfxRect control(struct UI *ui, const struct UIGadget *g)
{
    struct GfxRect r = inset(g->g_Rect, FOCUS);
    int32_t mid = (g->g_Rect.MinY + g->g_Rect.MaxY + 1) / 2, h = 0;

    switch (g->g_Kind) {
    case UI_TOGGLE:
        r.MinX = r.MaxX - TOGGLE_W + 1;
        h = TOGGLE_H;
        break;
    case UI_SLIDER:
        r.MinX = g->g_Rect.MinX + (g->g_Label ? label_w(ui, g) : FOCUS);
        h = KNOB_H;
        break;
    case UI_SEGMENTED:
        h = SEG_H;
        break;
    case UI_BUTTON:
        h = BUTTON_H;
        break;
    case UI_TEXTFIELD:
        h = FIELD_H;
        break;
    default:
        return r;
    }
    r.MinY = mid - h / 2;
    r.MaxY = r.MinY + h - 1;
    return r;
}

static uint32_t darker(uint32_t c)
{
    uint32_t r = (c >> 16 & 0xff) * 5 / 8, g = (c >> 8 & 0xff) * 5 / 8, b = (c & 0xff) * 5 / 8;

    return 0xff000000 | r << 16 | g << 8 | b;
}

static void draw_toggle(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    struct GfxRect c = control(ui, g), t;
    bool on = g->g_Value != 0, amber = g->g_Variant == TOGGLE_AMBER;
    uint32_t track = g->g_Disabled ? TRACK_DISABLED : amber ? (on ? AMBER_TOP : AMBER_OFF) : (on ? COBALT : TRACK_OFF);
    int32_t tx = on ? c.MaxX - 1 - THUMB_W : c.MinX + 2, ty = c.MinY + 2;

    label(ui, n, g);
    shape(ui, c, TOGGLE_H / 2, darker(track), track, 0, (struct GfxRect){ c.MinX, c.MinY, c.MaxX, c.MinY + 3 });
    t = (struct GfxRect){ tx, ty, tx + THUMB_W - 1, ty + THUMB_H - 1 };
    if (amber && g->g_Glyph && !g->g_Disabled) {
        uint64_t len = length(g->g_Glyph);
        int32_t x0 = on ? c.MinX : t.MaxX + 1, x1 = on ? t.MinX - 1 : c.MaxX;
        int32_t w = width(ui, ui->ui_Bold, g->g_Glyph, len);

        text(ui, ui->ui_Bold, on ? TEXT_DARK : TEXT_LIGHT, 0, (x0 + x1 + 1 - w) / 2,
             c.MinY + (TOGGLE_H - line_h(ui->ui_Bold)) / 2, g->g_Glyph, len);
    }
    if (g->g_Disabled) {
        rounded(ui, t, THUMB_H / 2, THUMB_DIS_TOP, THUMB_DIS_BOTTOM, 0);
        for (int32_t i = -3; i <= 3; i += 3)
            rect(ui, TEXT_MUTED, (t.MinX + t.MaxX) / 2 + i, t.MinY + 5, (t.MinX + t.MaxX) / 2 + i, t.MaxY - 5);
    } else {
        rounded(ui, t, THUMB_H / 2, THUMB_TOP, THUMB_BOTTOM, THUMB_EDGE);
    }
    if (ui->ui_Focus == n)
        ring(ui, c, TOGGLE_H / 2, 2);
}

/* A slider's thumb's middle for a value, and the range it runs over. */
static int32_t knob_x(const struct UIGadget *g, struct GfxRect c, int32_t value)
{
    int32_t x0 = c.MinX + KNOB_W / 2, x1 = c.MaxX - KNOB_W / 2;
    int64_t range = (int64_t)g->g_Max - g->g_Min;

    return range > 0 ? x0 + (int32_t)(((int64_t)value - g->g_Min) * (x1 - x0) / range) : x0;
}

static int32_t knob_value(const struct UIGadget *g, struct GfxRect c, int32_t x)
{
    int32_t x0 = c.MinX + KNOB_W / 2, x1 = c.MaxX - KNOB_W / 2;
    int64_t range = (int64_t)g->g_Max - g->g_Min;

    if (x1 <= x0 || x <= x0)
        return g->g_Min;
    if (x >= x1)
        return g->g_Max;
    return g->g_Min + (int32_t)(((int64_t)(x - x0) * range + (x1 - x0) / 2) / (x1 - x0));
}

static void draw_slider(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    struct GfxRect c = control(ui, g), k;
    int32_t x = knob_x(g, c, g->g_Value), ty = c.MinY + (KNOB_H - TRACK_H) / 2;
    struct GfxRect track = { c.MinX, ty, c.MaxX, ty + TRACK_H - 1 };

    label(ui, n, g);
    rounded(ui, track, TRACK_H / 2, SLIDER_TRACK, SLIDER_TRACK, 0);
    rect(ui, SLIDER_LIGHT, c.MinX + TRACK_H / 2, track.MaxY, c.MaxX - TRACK_H / 2, track.MaxY);
    if (x > c.MinX) {
        struct GfxRect f = { c.MinX, ty, x, ty + TRACK_H - 2 };

        rounded(ui, f, TRACK_H / 2, g->g_Disabled ? FILL_DISABLED : SLIDER_FILL,
                g->g_Disabled ? FILL_DISABLED : SLIDER_FILL, 0);
    }
    k = (struct GfxRect){ x - KNOB_W / 2, c.MinY, x - KNOB_W / 2 + KNOB_W - 1, c.MinY + KNOB_H - 1 };
    rounded(ui, k, 4, g->g_Disabled ? THUMB_DIS_TOP : KNOB, g->g_Disabled ? THUMB_DIS_BOTTOM : THUMB_BOTTOM,
            KNOB_EDGE);
    if (ui->ui_Focus == n)
        ring(ui, k, 4, 2);
}

/* A segment's left and right, of 'count' across c. */
static void segment(struct GfxRect c, uint32_t count, uint32_t i, int32_t *x0, int32_t *x1)
{
    int32_t w = (c.MaxX - c.MinX - 1) / (int32_t)(count ? count : 1);

    *x0 = c.MinX + 1 + (int32_t)i * w;
    *x1 = i + 1 == count ? c.MaxX - 1 : *x0 + w - 1;
}

static void draw_segmented(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    struct GfxRect c = control(ui, g);
    int32_t ty = c.MinY + (SEG_H - line_h(ui->ui_Sans)) / 2;

    rounded(ui, c, SEG_H / 2, SEG_TOP, SEG_BOTTOM, SEG_EDGE);
    for (uint32_t i = 0; i < g->g_Count; i++) {
        bool on = (int32_t)i == g->g_Value;
        const struct TextFont *f = on ? ui->ui_Bold : ui->ui_Sans;
        uint64_t len = length(g->g_Items[i]);
        int32_t x0, x1, w = width(ui, f, g->g_Items[i], len);

        segment(c, g->g_Count, i, &x0, &x1);
        if (on && !g->g_Disabled)
            rounded(ui, (struct GfxRect){ x0 + 1, c.MinY + 2, x1 - 1, c.MaxY - 2 }, SEG_H / 2 - 2, AMBER_TOP,
                    AMBER_BOTTOM, 0);
        else if (i > 0 && (int32_t)i - 1 != g->g_Value)
            rect(ui, SEG_LINE, x0, c.MinY + 6, x0, c.MaxY - 6);
        text(ui, f, g->g_Disabled ? TEXT_MUTED : on ? TEXT_DARK : 0xff1a2230, 0, (x0 + x1 + 1 - w) / 2, ty,
             g->g_Items[i], len);
    }
    if (ui->ui_Focus == n)
        ring(ui, c, SEG_H / 2, 2);
}

static void draw_button(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    struct GfxRect c = control(ui, g);
    bool down = ui->ui_Pressed == n && ui->ui_Inside;
    uint64_t len = length(g->g_Label);
    int32_t w = width(ui, ui->ui_Sans, g->g_Label, len);

    rounded(ui, c, 6, down ? AMBER_TOP : THUMB_TOP, down ? AMBER_BOTTOM : THUMB_BOTTOM, THUMB_EDGE);
    text(ui, ui->ui_Sans, g->g_Disabled ? TEXT_MUTED : TEXT_DARK, 0, (c.MinX + c.MaxX + 1 - w) / 2,
         c.MinY + (BUTTON_H - line_h(ui->ui_Sans)) / 2, g->g_Label, len);
    if (ui->ui_Focus == n)
        ring(ui, c, 6, 2);
}

static void draw_label(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    const struct pane *c = colours(ui, n);
    uint64_t len = length(g->g_Label);
    int32_t w = width(ui, ui->ui_Sans, g->g_Label, len), x = g->g_Rect.MinX;
    int32_t y = g->g_Rect.MinY + (g->g_Rect.MaxY - g->g_Rect.MinY + 1 - line_h(ui->ui_Sans)) / 2;

    if (g->g_Variant == ALIGN_CENTER)
        x = (g->g_Rect.MinX + g->g_Rect.MaxX + 1 - w) / 2;
    else if (g->g_Variant == ALIGN_RIGHT)
        x = g->g_Rect.MaxX + 1 - w;
    text(ui, ui->ui_Sans, g->g_Disabled ? TEXT_MUTED : c->text, g->g_Disabled ? 0 : c->under, x, y, g->g_Label,
         len);
}

static void draw_divider(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    const struct pane *c = colours(ui, n);
    int32_t y = (g->g_Rect.MinY + g->g_Rect.MaxY) / 2;

    rect(ui, c->groove_dark, g->g_Rect.MinX, y, g->g_Rect.MaxX, y);
    rect(ui, c->groove_light, g->g_Rect.MinX, y + 1, g->g_Rect.MaxX, y + 1);
}

static void draw_badge(struct UI *ui, struct UIGadget *g)
{
    const char *s = g->g_Label;
    int32_t y = g->g_Rect.MinY + BADGE_PADY;

    rounded(ui, g->g_Rect, 6, BADGE, BADGE, BADGE_EDGE);
    while (s && *s) {
        uint64_t len = 0;

        while (s[len] && s[len] != '\n')
            len++;
        text(ui, ui->ui_Mono, TEXT_LIGHT, 0, g->g_Rect.MaxX + 1 - BADGE_PADX - width(ui, ui->ui_Mono, s, len), y, s,
             len);
        y += line_h(ui->ui_Mono);
        s += len + (s[len] == '\n');
    }
}

/* A list's rows shown whole. */
static int32_t rows_shown(struct UI *ui, const struct UIGadget *g)
{
    int32_t h = control(ui, g).MaxY - control(ui, g).MinY - 1;

    return h >= ROW_H ? h / ROW_H : 1;
}

static void draw_list(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    const struct pane *c = colours(ui, n);
    struct GfxRect w = control(ui, g), in = inset(w, 1);
    struct RastPort *rp = ui->ui_RPort;
    struct GfxRect saved = rp->rp_Clip;

    rect(ui, ui->ui_Focus == n ? FOCUS_RING : c->well_edge, w.MinX, w.MinY, w.MaxX, w.MaxY);
    rect(ui, c->well, in.MinX, in.MinY, in.MaxX, in.MaxY);
    rp->rp_Clip = meet(saved, in);
    for (int32_t i = g->g_Top; i >= 0 && i < (int32_t)g->g_Count; i++) {
        int32_t y = in.MinY + (i - g->g_Top) * ROW_H;
        bool on = i == g->g_Value;

        if (y > in.MaxY)
            break;
        if (on) {
            struct GfxGradient gg = { { in.MinX, y, in.MaxX, y + ROW_H - 1 }, COBALT_TOP, COBALT_BOTTOM, 0 };

            ui->ui_Gfx->FillGradient(rp, &gg, nullptr, 0);
        }
        text(ui, ui->ui_Sans, g->g_Disabled ? TEXT_MUTED : on ? TEXT_LIGHT : c->well_text, 0, in.MinX + 8,
             y + (ROW_H - line_h(ui->ui_Sans)) / 2, g->g_Items[i], length(g->g_Items[i]));
    }
    rp->rp_Clip = saved;
}

static uint32_t field_len(const struct UIGadget *g)
{
    uint32_t n = 0;

    while (g->g_Buffer && n + 1 < g->g_Size && g->g_Buffer[n])
        n++;
    return n;
}

/* A text field's scroll, so its cursor's in sight. */
static void field_scroll(struct UI *ui, struct UIGadget *g)
{
    struct GfxRect c = control(ui, g);
    int32_t room = c.MaxX - c.MinX + 1 - 2 * FIELD_PAD, x = width(ui, ui->ui_Sans, g->g_Buffer, (uint64_t)g->g_Cursor);

    if (x - g->g_Top > room - 1)
        g->g_Top = x - room + 1;
    if (x < g->g_Top)
        g->g_Top = x;
    if (g->g_Top < 0)
        g->g_Top = 0;
}

static void draw_field(struct UI *ui, uint32_t n, struct UIGadget *g)
{
    const struct pane *c = colours(ui, n);
    struct GfxRect w = control(ui, g);
    struct RastPort *rp = ui->ui_RPort;
    struct GfxRect saved = rp->rp_Clip;
    int32_t x = w.MinX + FIELD_PAD - g->g_Top, y = w.MinY + (FIELD_H - line_h(ui->ui_Sans)) / 2;

    rounded(ui, w, 4, c->well, c->well, ui->ui_Focus == n ? FOCUS_RING : c->well_edge);
    rp->rp_Clip = meet(saved, (struct GfxRect){ w.MinX + 2, saved.MinY, w.MaxX - 2, saved.MaxY });
    text(ui, ui->ui_Sans, g->g_Disabled ? TEXT_MUTED : c->well_text, 0, x, y, g->g_Buffer, field_len(g));
    if (ui->ui_Focus == n) {
        int32_t cx = x + width(ui, ui->ui_Sans, g->g_Buffer, (uint64_t)g->g_Cursor);

        rect(ui, c->well_text, cx, w.MinY + 4, cx, w.MaxY - 4);
    }
    rp->rp_Clip = saved;
}

static void draw_split(struct UI *ui, struct UIGadget *g)
{
    int32_t x = G(ui, g->g_Child)->g_Rect.MaxX + 1;

    rect(ui, SPLIT_DARK, x, g->g_Rect.MinY, x, g->g_Rect.MaxY);
    rect(ui, SPLIT_LIGHT, x + 1, g->g_Rect.MinY, x + 1, g->g_Rect.MaxY);
}

/* A gadget and its children, on what's drawn under them already. */
static void draw_tree(struct UI *ui, uint32_t n)
{
    struct UIGadget *g = G(ui, n);

    if (g->g_Rect.MaxX < g->g_Rect.MinX || g->g_Rect.MaxY < g->g_Rect.MinY)
        return;
    if (g->g_Pane)
        pane_fill(ui, n);
    switch (g->g_Kind) {
    case UI_SPLITPANE:
        if (g->g_Child && G(ui, g->g_Child)->g_Next)
            draw_split(ui, g);
        break;
    case UI_DIVIDER:
        draw_divider(ui, n, g);
        break;
    case UI_TOGGLE:
        draw_toggle(ui, n, g);
        break;
    case UI_SLIDER:
        draw_slider(ui, n, g);
        break;
    case UI_SEGMENTED:
        draw_segmented(ui, n, g);
        break;
    case UI_HUDBADGE:
        draw_badge(ui, g);
        break;
    case UI_BUTTON:
        draw_button(ui, n, g);
        break;
    case UI_LABEL:
        draw_label(ui, n, g);
        break;
    case UI_LIST:
        draw_list(ui, n, g);
        break;
    case UI_TEXTFIELD:
        draw_field(ui, n, g);
        break;
    }
    for (uint32_t c = g->g_Child; c; c = G(ui, c)->g_Next)
        draw_tree(ui, c);
}

static void damage(struct UI *ui, struct GfxRect r)
{
    struct GfxRect *d = ui->ui_Damage;

    r.MinX = r.MinX < 0 ? 0 : r.MinX;
    r.MinY = r.MinY < 0 ? 0 : r.MinY;
    r.MaxX = r.MaxX >= (int32_t)ui->ui_Width ? (int32_t)ui->ui_Width - 1 : r.MaxX;
    r.MaxY = r.MaxY >= (int32_t)ui->ui_Height ? (int32_t)ui->ui_Height - 1 : r.MaxY;
    if (r.MaxX < r.MinX || r.MaxY < r.MinY)
        return;
    for (uint32_t i = 0; i < ui->ui_NumDamage; i++)
        if (d[i].MinX <= r.MinX && d[i].MinY <= r.MinY && d[i].MaxX >= r.MaxX && d[i].MaxY >= r.MaxY)
            return;
    if (ui->ui_NumDamage == UI_DAMAGE) {
        for (uint32_t i = 1; i < UI_DAMAGE; i++) {
            d[0].MinX = d[i].MinX < d[0].MinX ? d[i].MinX : d[0].MinX;
            d[0].MinY = d[i].MinY < d[0].MinY ? d[i].MinY : d[0].MinY;
            d[0].MaxX = d[i].MaxX > d[0].MaxX ? d[i].MaxX : d[0].MaxX;
            d[0].MaxY = d[i].MaxY > d[0].MaxY ? d[i].MaxY : d[0].MaxY;
        }
        ui->ui_NumDamage = 1;
    }
    d[ui->ui_NumDamage++] = r;
}

/* One gadget drawn again, alone, on its pane: its rectangle damaged. */
static void redraw(struct UI *ui, uint32_t n)
{
    struct UIGadget *g = G(ui, n);
    struct RastPort *rp = ui->ui_RPort;
    struct GfxRect saved, r = g->g_Rect;

    if (!n || !rp || !ui->ui_Width || r.MaxX < r.MinX || r.MaxY < r.MinY)
        return;
    saved = rp->rp_Clip;
    rp->rp_Clip = meet(saved, r);
    if (!g->g_Pane)
        pane_fill(ui, pane_of(ui, n));
    draw_tree(ui, n);
    rp->rp_Clip = saved;
    damage(ui, r);
}

/* --- Gadgets made and changed. --- */

static void clamp(struct UIGadget *g)
{
    int32_t lo = 0, hi = 0;

    switch (g->g_Kind) {
    case UI_TOGGLE:
        hi = 1;
        break;
    case UI_SLIDER:
        if (g->g_Max < g->g_Min)
            g->g_Max = g->g_Min;
        lo = g->g_Min, hi = g->g_Max;
        break;
    case UI_SEGMENTED:
        hi = g->g_Count ? (int32_t)g->g_Count - 1 : 0;
        break;
    case UI_LIST:
        lo = -1, hi = (int32_t)g->g_Count - 1;
        g->g_Top = g->g_Top > hi ? hi : g->g_Top;
        g->g_Top = g->g_Top < 0 ? 0 : g->g_Top;
        break;
    case UI_TEXTFIELD:
        g->g_Cursor = g->g_Cursor < 0 ? 0 : g->g_Cursor > (int32_t)field_len(g) ? (int32_t)field_len(g) : g->g_Cursor;
        return;
    default:
        return;
    }
    g->g_Value = g->g_Value < lo ? lo : g->g_Value > hi ? hi : g->g_Value;
}

static void apply(struct UIGadget *g, const struct TagItem *tags)
{
    const struct TagItem *t;

    while ((t = NextTagItem(&tags))) {
        uint64_t d = t->ti_Data;

        switch (t->ti_Tag) {
        case UIA_Label: g->g_Label = (const char *)d; break;
        case UIA_Value: g->g_Value = (int32_t)d; break;
        case UIA_Min: g->g_Min = (int32_t)d; break;
        case UIA_Max: g->g_Max = (int32_t)d; break;
        case UIA_Items: g->g_Items = (const char *const *)d; break;
        case UIA_Count: g->g_Count = (uint32_t)d; break;
        case UIA_Buffer: g->g_Buffer = (char *)d; break;
        case UIA_Size: g->g_Size = (uint32_t)d; break;
        case UIA_Weight: g->g_Weight = (uint32_t)d; break;
        case UIA_Spacing: g->g_Spacing = (uint32_t)d; break;
        case UIA_Margin: g->g_Margin = (uint32_t)d; break;
        case UIA_Width: g->g_Width = (uint32_t)d; break;
        case UIA_Height: g->g_Height = (uint32_t)d; break;
        case UIA_Variant: g->g_Variant = (uint8_t)d; break;
        case UIA_Disabled: g->g_Disabled = d != 0; break;
        case UIA_Align: g->g_Align = (uint8_t)d; break;
        case UIA_Pane: g->g_Pane = (uint8_t)d; break;
        case UIA_Glyph: g->g_Glyph = (const char *)d; break;
        }
    }
    if (!g->g_Items)
        g->g_Count = 0;
    if (!g->g_Buffer)
        g->g_Size = 0;
}

int64_t InitUI(struct UI *ui, const struct GfxIFace *gfx)
{
    memset(ui, 0, sizeof *ui);
    ui->ui_Gfx = gfx;
    ui->ui_Sans = gfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 });
    ui->ui_Bold = gfx->OpenTextFont(&(struct TextAttr){ "System Sans", 13, FSF_BOLD, 0 });
    ui->ui_Mono = gfx->OpenTextFont(&(struct TextAttr){ "System Mono", 13, 0, 0 });
    if (!ui->ui_Sans || !ui->ui_Bold || !ui->ui_Mono) {
        FreeUI(ui);
        return ERR_NOTFOUND;
    }
    return 0;
}

void FreeUI(struct UI *ui)
{
    struct TextFont *f[3] = { ui->ui_Sans, ui->ui_Bold, ui->ui_Mono };

    for (unsigned i = 0; i < 3; i++)
        if (f[i])
            ui->ui_Gfx->CloseTextFont(f[i]);
    ui->ui_Sans = ui->ui_Bold = ui->ui_Mono = nullptr;
}

uint32_t AddGadget(struct UI *ui, uint32_t parent, uint32_t kind, const struct TagItem *tags)
{
    uint32_t n = ui->ui_Count + 1, kids = 0;
    struct UIGadget *g;

    if (n > UI_GADGETS || kind < UI_ROW || kind > UI_TEXTFIELD || (parent == 0) != (n == 1) ||
        parent >= n || (parent && !group(G(ui, parent)->g_Kind)))
        return 0;
    if (parent) {
        for (uint32_t c = G(ui, parent)->g_Child; c; c = G(ui, c)->g_Next)
            kids++;
        if (G(ui, parent)->g_Kind == UI_SPLITPANE && kids == 2)
            return 0;
    }
    g = G(ui, n);
    memset(g, 0, sizeof *g);
    g->g_Kind = (uint8_t)kind;
    g->g_Parent = (uint8_t)parent;
    g->g_Spacing = 8;
    g->g_Max = 100;
    if (kind == UI_LIST)
        g->g_Value = -1;
    if (parent && G(ui, parent)->g_Kind == UI_SPLITPANE)
        g->g_Pane = kids ? UIPANE_LIGHT : UIPANE_DARK;
    apply(g, tags);
    clamp(g);
    if (kind == UI_TEXTFIELD)
        g->g_Cursor = (int32_t)field_len(g);
    if (parent) {
        uint32_t last = 0;

        for (uint32_t c = G(ui, parent)->g_Child; c; c = G(ui, c)->g_Next)
            last = c;
        if (last)
            G(ui, last)->g_Next = (uint8_t)n;
        else
            G(ui, parent)->g_Child = (uint8_t)n;
    }
    ui->ui_Count = n;
    return n;
}

/* --- Layout. --- */

static void measure(struct UI *ui, uint32_t n)
{
    struct UIGadget *g = G(ui, n);
    int32_t w = 0, h = 0, k = 0;

    for (uint32_t c = g->g_Child; c; c = G(ui, c)->g_Next) {
        struct UIGadget *cg = G(ui, c);

        measure(ui, c);
        if (g->g_Kind == UI_COLUMN) {
            w = cg->g_NatW > w ? cg->g_NatW : w;
            h += cg->g_NatH;
        } else {
            h = cg->g_NatH > h ? cg->g_NatH : h;
            w += cg->g_NatW;
        }
        k++;
    }
    switch (g->g_Kind) {
    case UI_ROW:
    case UI_COLUMN:
        if (k > 1)
            *(g->g_Kind == UI_ROW ? &w : &h) += (k - 1) * (int32_t)g->g_Spacing;
        w += 2 * (int32_t)g->g_Margin;
        h += 2 * (int32_t)g->g_Margin;
        break;
    case UI_SPLITPANE:
        w += GROOVE;
        break;
    case UI_DIVIDER:
        h = GROOVE;
        break;
    case UI_TOGGLE:
        w = label_w(ui, g) + TOGGLE_W + 2 * FOCUS;
        h = TOGGLE_H + 2 * FOCUS;
        break;
    case UI_SLIDER:
        w = (g->g_Label ? label_w(ui, g) : FOCUS) + SLIDER_W + FOCUS;
        h = KNOB_H + 2 * FOCUS;
        break;
    case UI_SEGMENTED:
        for (uint32_t i = 0; i < g->g_Count; i++) {
            int32_t iw = width(ui, ui->ui_Bold, g->g_Items[i], length(g->g_Items[i])) + SEG_PAD;

            w = iw > w ? iw : w;
        }
        w = w * (int32_t)g->g_Count + 2 + 2 * FOCUS;
        h = SEG_H + 2 * FOCUS;
        break;
    case UI_HUDBADGE:
        for (const char *s = g->g_Label; s && *s;) {
            uint64_t len = 0;
            int32_t lw;

            while (s[len] && s[len] != '\n')
                len++;
            lw = width(ui, ui->ui_Mono, s, len);
            w = lw > w ? lw : w;
            h += line_h(ui->ui_Mono);
            s += len + (s[len] == '\n');
        }
        w += 2 * BADGE_PADX;
        h += 2 * BADGE_PADY;
        break;
    case UI_BUTTON:
        w = width(ui, ui->ui_Sans, g->g_Label, length(g->g_Label)) + BUTTON_PAD;
        w = (w > BUTTON_W ? w : BUTTON_W) + 2 * FOCUS;
        h = BUTTON_H + 2 * FOCUS;
        break;
    case UI_LABEL:
        w = width(ui, ui->ui_Sans, g->g_Label, length(g->g_Label));
        h = line_h(ui->ui_Sans);
        break;
    case UI_LIST:
        w = LIST_W + 2 * FOCUS;
        h = LIST_ROWS * ROW_H + 2 + 2 * FOCUS;
        break;
    case UI_TEXTFIELD:
        w = FIELD_W + 2 * FOCUS;
        h = FIELD_H + 2 * FOCUS;
        break;
    }
    g->g_NatW = w > (int32_t)g->g_Width ? w : (int32_t)g->g_Width;
    g->g_NatH = h > (int32_t)g->g_Height ? h : (int32_t)g->g_Height;
}

static void place(struct UI *ui, uint32_t n, struct GfxRect r);

static void place_group(struct UI *ui, struct UIGadget *g, struct GfxRect r)
{
    bool row = g->g_Kind == UI_ROW;
    struct GfxRect in = inset(r, (int32_t)g->g_Margin);
    int32_t main = row ? in.MaxX - in.MinX + 1 : in.MaxY - in.MinY + 1;
    int32_t cross = row ? in.MaxY - in.MinY + 1 : in.MaxX - in.MinX + 1;
    int32_t natural = 0, extra, pos = row ? in.MinX : in.MinY, given = 0;
    uint32_t weights = 0, k = 0, wsum = 0;

    for (uint32_t c = g->g_Child; c; c = G(ui, c)->g_Next) {
        natural += row ? G(ui, c)->g_NatW : G(ui, c)->g_NatH;
        weights += G(ui, c)->g_Weight;
        k++;
    }
    extra = main - natural - (k > 1 ? (int32_t)(k - 1) * (int32_t)g->g_Spacing : 0);
    extra = extra < 0 ? 0 : extra;
    for (uint32_t c = g->g_Child; c; c = G(ui, c)->g_Next) {
        struct UIGadget *cg = G(ui, c);
        uint8_t align = cg->g_Align ? cg->g_Align : g->g_Align ? g->g_Align : row ? UIALIGN_CENTER : UIALIGN_FILL;
        int32_t size = row ? cg->g_NatW : cg->g_NatH, nat = row ? cg->g_NatH : cg->g_NatW, at, across;

        if (weights && cg->g_Weight) {
            int32_t share;

            wsum += cg->g_Weight;
            share = (int32_t)((int64_t)extra * wsum / weights) - given;
            given += share;
            size += share;
        }
        across = align == UIALIGN_FILL || nat > cross ? cross : nat;
        at = align == UIALIGN_CENTER ? (cross - across) / 2 : align == UIALIGN_END ? cross - across : 0;
        if (row)
            place(ui, c, (struct GfxRect){ pos, in.MinY + at, pos + size - 1, in.MinY + at + across - 1 });
        else
            place(ui, c, (struct GfxRect){ in.MinX + at, pos, in.MinX + at + across - 1, pos + size - 1 });
        pos += size + (int32_t)g->g_Spacing;
    }
}

/* A split pane's left width, kept where both sides fit. */
static int32_t split_at(struct UI *ui, const struct UIGadget *g, int32_t left)
{
    const struct UIGadget *a = G(ui, g->g_Child), *b = a->g_Next ? G(ui, a->g_Next) : nullptr;
    int32_t w = g->g_Rect.MaxX - g->g_Rect.MinX + 1, hi = w - GROOVE - (b ? b->g_NatW : 0);

    left = left > hi ? hi : left;
    return left < a->g_NatW ? a->g_NatW : left;
}

static void place(struct UI *ui, uint32_t n, struct GfxRect r)
{
    struct UIGadget *g = G(ui, n);

    g->g_Rect = r;
    if (g->g_Kind == UI_ROW || g->g_Kind == UI_COLUMN) {
        place_group(ui, g, r);
    } else if (g->g_Kind == UI_SPLITPANE && g->g_Child) {
        struct UIGadget *a = G(ui, g->g_Child);
        int32_t left;

        /* Where it's asked to be, kept: a window too small for it moves it only while it is. */
        if (!g->g_Value)
            g->g_Value = a->g_NatW;
        left = split_at(ui, g, g->g_Value);
        place(ui, g->g_Child, (struct GfxRect){ r.MinX, r.MinY, r.MinX + left - 1, r.MaxY });
        if (a->g_Next)
            place(ui, a->g_Next, (struct GfxRect){ r.MinX + left + GROOVE, r.MinY, r.MaxX, r.MaxY });
    } else if (g->g_Kind == UI_TEXTFIELD) {
        field_scroll(ui, g);
    }
}

void LayoutUI(struct UI *ui, uint32_t width, uint32_t height)
{
    ui->ui_Width = width;
    ui->ui_Height = height;
    if (!ui->ui_Count)
        return;
    measure(ui, 1);
    place(ui, 1, (struct GfxRect){ 0, 0, (int32_t)width - 1, (int32_t)height - 1 });
}

void DrawUI(struct UI *ui)
{
    struct RastPort *rp = ui->ui_RPort;
    struct GfxRect all = { 0, 0, (int32_t)ui->ui_Width - 1, (int32_t)ui->ui_Height - 1 }, saved;

    if (!rp || !ui->ui_Width || !ui->ui_Height)
        return;
    saved = rp->rp_Clip;
    rp->rp_Clip = all;
    pane_fill(ui, 0);
    if (ui->ui_Count)
        draw_tree(ui, 1);
    rp->rp_Clip = saved;
    ui->ui_NumDamage = 0;
    damage(ui, all);
}

void SetGadgetAttrsA(struct UI *ui, uint32_t n, const struct TagItem *tags)
{
    struct UIGadget *g;

    if (!n || n > ui->ui_Count)
        return;
    g = G(ui, n);
    apply(g, tags);
    clamp(g);
    if (!ui->ui_Width)
        return;
    if (g->g_Kind == UI_SPLITPANE || g->g_Kind == UI_TEXTFIELD)
        place(ui, n, g->g_Rect);
    redraw(ui, n);
}

/* --- Events. --- */

static uint32_t changed(struct UIChange *ch, uint32_t n, int32_t value, uint32_t flags)
{
    *ch = (struct UIChange){ n, value, flags };
    return n;
}

static void focus(struct UI *ui, uint32_t n)
{
    uint32_t old = ui->ui_Focus;

    if (n == old)
        return;
    ui->ui_Focus = n;
    redraw(ui, old);
    redraw(ui, n);
}

/* The deepest gadget at (x, y): a split pane itself on its groove. */
static uint32_t hit(struct UI *ui, int32_t x, int32_t y)
{
    uint32_t n = 1, found = 0;

    while (n && within(G(ui, n)->g_Rect, x, y)) {
        struct UIGadget *g = G(ui, n);

        found = n;
        if (g->g_Kind == UI_SPLITPANE && g->g_Child) {
            int32_t gx = G(ui, g->g_Child)->g_Rect.MaxX + 1;

            if (x >= gx - 2 && x <= gx + GROOVE + 1)
                break;
        }
        n = 0;
        for (uint32_t c = g->g_Child; c; c = G(ui, c)->g_Next)
            if (within(G(ui, c)->g_Rect, x, y)) {
                n = c;
                break;
            }
    }
    return found;
}

/* A list's selection, and its rows scrolled to show it. */
static void list_select(struct UI *ui, struct UIGadget *g, int32_t row)
{
    int32_t shown = rows_shown(ui, g);

    g->g_Value = row;
    if (row >= 0 && row < g->g_Top)
        g->g_Top = row;
    if (row >= g->g_Top + shown)
        g->g_Top = row - shown + 1;
}

/* A text field's cursor at the character boundary nearest x. */
static int32_t field_at(struct UI *ui, struct UIGadget *g, int32_t x)
{
    int32_t best = 0, best_d = INT32_MAX, len = (int32_t)field_len(g);

    x -= control(ui, g).MinX + FIELD_PAD - g->g_Top;
    for (int32_t i = 0; i <= len; i++) {
        int32_t d;

        if (i < len && (g->g_Buffer[i] & 0xc0) == 0x80)
            continue;
        d = width(ui, ui->ui_Sans, g->g_Buffer, (uint64_t)i) - x;
        d = d < 0 ? -d : d;
        if (d < best_d)
            best = i, best_d = d;
    }
    return best;
}

static uint32_t press(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *ch)
{
    int32_t x = ev->iev_MouseX, y = ev->iev_MouseY;
    uint32_t n = hit(ui, x, y);
    struct UIGadget *g = G(ui, n);

    if (!n || g->g_Disabled)
        return 0;
    if (focusable(g->g_Kind))
        focus(ui, n);
    ui->ui_Pressed = n;
    ui->ui_Inside = true;
    switch (g->g_Kind) {
    case UI_SPLITPANE:
        ui->ui_GrabX = x - (G(ui, g->g_Child)->g_Rect.MaxX + 1);
        return 0;
    case UI_SLIDER: {
        struct GfxRect c = control(ui, g);
        int32_t kx = knob_x(g, c, g->g_Value), v;

        if (x >= kx - KNOB_W / 2 && x < kx - KNOB_W / 2 + KNOB_W) {
            ui->ui_GrabX = x - kx;
            return 0;
        }
        ui->ui_GrabX = 0;
        if ((v = knob_value(g, c, x)) == g->g_Value)
            return 0;
        g->g_Value = v;
        redraw(ui, n);
        return changed(ch, n, v, UICF_DRAG);
    }
    case UI_SEGMENTED: {
        struct GfxRect c = control(ui, g);

        for (uint32_t i = 0; i < g->g_Count; i++) {
            int32_t x0, x1;

            segment(c, g->g_Count, i, &x0, &x1);
            if (x >= x0 && x <= x1 && (int32_t)i != g->g_Value) {
                g->g_Value = (int32_t)i;
                redraw(ui, n);
                return changed(ch, n, g->g_Value, 0);
            }
        }
        return 0;
    }
    case UI_LIST: {
        struct GfxRect in = inset(control(ui, g), 1);
        int32_t row = g->g_Top + (y - in.MinY) / ROW_H;
        bool twice = ui->ui_ClickGadget == n && ui->ui_ClickRow == row &&
                     ev->iev_Time - ui->ui_ClickTime <= UI_DOUBLE_NS;

        if (y < in.MinY || y > in.MaxY || row >= (int32_t)g->g_Count)
            return 0;
        ui->ui_ClickGadget = twice ? 0 : n;
        ui->ui_ClickRow = row;
        ui->ui_ClickTime = ev->iev_Time;
        if (row == g->g_Value && !twice)
            return 0;
        list_select(ui, g, row);
        redraw(ui, n);
        return changed(ch, n, row, twice ? UICF_DOUBLE : 0);
    }
    case UI_TEXTFIELD:
        g->g_Cursor = field_at(ui, g, x);
        field_scroll(ui, g);
        redraw(ui, n);
        return 0;
    case UI_BUTTON:
        redraw(ui, n);
        return 0;
    case UI_TOGGLE:
        return 0;
    }
    ui->ui_Pressed = 0;
    return 0;
}

static uint32_t move(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *ch)
{
    uint32_t n = ui->ui_Pressed;
    struct UIGadget *g = G(ui, n);
    int32_t x = ev->iev_MouseX, v;
    bool in;

    if (!n)
        return 0;
    switch (g->g_Kind) {
    case UI_SPLITPANE:
        if ((v = split_at(ui, g, x - ui->ui_GrabX - g->g_Rect.MinX)) == g->g_Value)
            return 0;
        g->g_Value = v;
        place(ui, n, g->g_Rect);
        redraw(ui, n);
        return changed(ch, n, v, UICF_DRAG);
    case UI_SLIDER:
        if ((v = knob_value(g, control(ui, g), x - ui->ui_GrabX)) == g->g_Value)
            return 0;
        g->g_Value = v;
        redraw(ui, n);
        return changed(ch, n, v, UICF_DRAG);
    case UI_BUTTON:
    case UI_TOGGLE:
        in = within(g->g_Rect, x, ev->iev_MouseY);
        if (in != ui->ui_Inside) {
            ui->ui_Inside = in;
            if (g->g_Kind == UI_BUTTON)
                redraw(ui, n);
        }
        return 0;
    }
    return 0;
}

static uint32_t release(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *ch)
{
    uint32_t n = ui->ui_Pressed;
    struct UIGadget *g = G(ui, n);
    bool in = n && within(g->g_Rect, ev->iev_MouseX, ev->iev_MouseY);

    ui->ui_Pressed = 0;
    ui->ui_Inside = false;
    if (!n)
        return 0;
    if (g->g_Kind == UI_BUTTON) {
        redraw(ui, n);
        return in ? changed(ch, n, 1, 0) : 0;
    }
    if (g->g_Kind == UI_TOGGLE && in) {
        g->g_Value = !g->g_Value;
        redraw(ui, n);
        return changed(ch, n, g->g_Value, 0);
    }
    if (g->g_Kind == UI_SLIDER || g->g_Kind == UI_SPLITPANE)
        return changed(ch, n, g->g_Value, 0);
    return 0;
}

/* Focus to the next gadget that takes it (or the one before), round. */
static void tab(struct UI *ui, bool back)
{
    uint32_t n = ui->ui_Focus;

    for (uint32_t i = 0; i < ui->ui_Count; i++) {
        n = back ? (n <= 1 ? ui->ui_Count : n - 1) : (n >= ui->ui_Count ? 1 : n + 1);
        if (focusable(G(ui, n)->g_Kind) && !G(ui, n)->g_Disabled) {
            focus(ui, n);
            return;
        }
    }
}

static uint32_t utf8_back(const char *s, uint32_t i)
{
    while (i > 0 && (s[--i] & 0xc0) == 0x80)
        ;
    return i;
}

static uint32_t utf8_on(const char *s, uint32_t i, uint32_t len)
{
    while (i < len && (s[++i] & 0xc0) == 0x80)
        ;
    return i;
}

/* A character typed in a text field: edited, and drawn again. */
static uint32_t edit(struct UI *ui, uint32_t n, struct UIGadget *g, uint32_t c, struct UIChange *ch)
{
    char *b = g->g_Buffer;
    uint32_t len = field_len(g), at = (uint32_t)g->g_Cursor, k = 0, cut;
    uint8_t u[4];

    if (!b || !g->g_Size)
        return 0;
    if (c == '\n' || c == '\r')
        return changed(ch, n, (int32_t)len, UICF_ENTER);
    if (c == '\b' || c == 0x7f) {
        uint32_t from = c == '\b' ? utf8_back(b, at) : at, to = c == '\b' ? at : utf8_on(b, at, len);

        if (from == to)
            return 0;
        memmove(b + from, b + to, len - to + 1);
        g->g_Cursor = (int32_t)from;
    } else if (c == 0x15) {             /* Ctrl-U: the whole line */
        b[0] = 0;
        g->g_Cursor = 0;
    } else if (c >= 0x20 && c < 0x110000) {
        if (c < 0x80)
            u[k++] = (uint8_t)c;
        else if (c < 0x800)
            u[k++] = (uint8_t)(0xc0 | c >> 6), u[k++] = (uint8_t)(0x80 | (c & 0x3f));
        else if (c < 0x10000)
            u[k++] = (uint8_t)(0xe0 | c >> 12), u[k++] = (uint8_t)(0x80 | (c >> 6 & 0x3f)),
            u[k++] = (uint8_t)(0x80 | (c & 0x3f));
        else
            u[k++] = (uint8_t)(0xf0 | c >> 18), u[k++] = (uint8_t)(0x80 | (c >> 12 & 0x3f)),
            u[k++] = (uint8_t)(0x80 | (c >> 6 & 0x3f)), u[k++] = (uint8_t)(0x80 | (c & 0x3f));
        if (len + k + 1 > g->g_Size)
            return 0;
        memmove(b + at + k, b + at, len - at + 1);
        for (cut = 0; cut < k; cut++)
            b[at + cut] = (char)u[cut];
        g->g_Cursor = (int32_t)(at + k);
    } else {
        return 0;
    }
    field_scroll(ui, g);
    redraw(ui, n);
    return 0;
}

static uint32_t typed(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *ch)
{
    uint32_t n = ui->ui_Focus, c = ev->iev_Char ? ev->iev_Char : ev->iev_Code;
    struct UIGadget *g = G(ui, n);

    if (c == '\t') {
        tab(ui, (ev->iev_Qualifier & IEQ_SHIFT) != 0);
        return 0;
    }
    if (!n || g->g_Disabled)
        return 0;
    switch (g->g_Kind) {
    case UI_TEXTFIELD:
        return edit(ui, n, g, c, ch);
    case UI_TOGGLE:
        if (c != ' ' && c != '\n' && c != '\r')
            return 0;
        g->g_Value = !g->g_Value;
        redraw(ui, n);
        return changed(ch, n, g->g_Value, 0);
    case UI_BUTTON:
        return c == ' ' || c == '\n' || c == '\r' ? changed(ch, n, 1, 0) : 0;
    case UI_LIST:
        return (c == '\n' || c == '\r') && g->g_Value >= 0 ? changed(ch, n, g->g_Value, UICF_DOUBLE) : 0;
    }
    return 0;
}

static uint32_t key(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *ch)
{
    uint32_t n = ui->ui_Focus;
    struct UIGadget *g = G(ui, n);
    uint16_t k = ev->iev_Code;
    int32_t v;

    if (!n || g->g_Disabled)
        return 0;
    v = g->g_Value;
    switch (g->g_Kind) {
    case UI_TEXTFIELD: {
        uint32_t len = field_len(g), at = (uint32_t)g->g_Cursor;

        at = k == KEY_LEFT ? utf8_back(g->g_Buffer, at) : k == KEY_RIGHT ? utf8_on(g->g_Buffer, at, len)
           : k == KEY_HOME ? 0 : k == KEY_END ? len : at;
        if (at != (uint32_t)g->g_Cursor) {
            g->g_Cursor = (int32_t)at;
            field_scroll(ui, g);
            redraw(ui, n);
        }
        return 0;
    }
    case UI_SLIDER: {
        int32_t step = ev->iev_Qualifier & IEQ_SHIFT ? (g->g_Max - g->g_Min) / 10 : 1;

        step = step < 1 ? 1 : step;
        v = k == KEY_LEFT || k == KEY_DOWN ? v - step : k == KEY_RIGHT || k == KEY_UP ? v + step
          : k == KEY_HOME ? g->g_Min : k == KEY_END ? g->g_Max : v;
        break;
    }
    case UI_SEGMENTED:
        v = k == KEY_LEFT ? v - 1 : k == KEY_RIGHT ? v + 1 : v;
        break;
    case UI_LIST:
        v = k == KEY_UP ? (v > 0 ? v - 1 : 0) : k == KEY_DOWN ? v + 1 : k == KEY_HOME ? 0
          : k == KEY_END ? (int32_t)g->g_Count - 1 : v;
        break;
    default:
        return 0;
    }
    {
        int32_t was = g->g_Value;

        g->g_Value = v;
        clamp(g);
        if (g->g_Kind == UI_LIST)
            list_select(ui, g, g->g_Value);
        if (g->g_Value == was)
            return 0;
    }
    redraw(ui, n);
    return changed(ch, n, g->g_Value, 0);
}

uint32_t HandleUIEvent(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *ch)
{
    *ch = (struct UIChange){};
    if (!ui->ui_Width || !ui->ui_Count)
        return 0;
    switch (ev->iev_Class) {
    case IDCMP_MOUSEBUTTONS:
        return ev->iev_Code == SELECTDOWN ? press(ui, ev, ch) : ev->iev_Code == SELECTUP ? release(ui, ev, ch) : 0;
    case IDCMP_MOUSEMOVE:
        return move(ui, ev, ch);
    case IDCMP_VANILLAKEY:
        return typed(ui, ev, ch);
    case IDCMP_RAWKEY:
        return ev->iev_Code & RAWKEY_UP ? 0 : key(ui, ev, ch);
    }
    return 0;
}
