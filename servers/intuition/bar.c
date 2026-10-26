/*
 * servers/intuition/bar.c -- the screen bar, its menus and requesters
 * (V8 step 4)
 *
 * §13.2's colours: the bar's metallic gradient with its white top edge
 * and dark bottom one, its text dark with a light etch a pixel below;
 * menus and requesters on the light silver pane, a highlighted item or
 * an open title in cobalt, buttons raised in thumb silver and amber when
 * pressed. Shapes are gfx.library's paths: the mark a ring, the clock a
 * ring with hands, a check mark, a key's diamond, rounded buttons.
 */
#include "bar.h"

constexpr int32_t BAR_H = INTUI_SCREENBAR;
constexpr uint32_t BAR_TOP = 0xffdce3ef, BAR_BOTTOM = 0xff9aa8c0;
constexpr uint32_t BAR_EDGE_LIGHT = 0xffffffff, BAR_EDGE_DARK = 0xff141a26, BAR_ETCH = 0xffe9eef5;
constexpr uint32_t PANE_TOP = 0xffd8dee8, PANE_BOTTOM = 0xffb8c2d1, PANE_TEXT = 0xff1a2230;
constexpr uint32_t PANE_BORDER = 0xff6e7b90, PANE_GROOVE_DARK = 0xff8e9ab0;
constexpr uint32_t HOT_TOP = 0xff3b82f6, HOT_BOTTOM = 0xff2563eb;
constexpr uint32_t THUMB_TOP = 0xfff4f7fb, THUMB_BOTTOM = 0xffcfd8e6;
constexpr uint32_t AMBER_TOP = 0xffeb861e, AMBER_BOTTOM = 0xffd46b0e;
constexpr uint32_t MARK_TOP = 0xff8bb4e8, MARK_BOTTOM = 0xff2563eb;

constexpr int32_t MARK_X = 10, MARK_SIZE = 18, NAME_X = 34, TITLE_PAD = 10;
constexpr int32_t ITEM_H = 22, BAR_ITEM_H = 9, MENU_PAD = 4, CHECK_W = 26, KEY_W = 34, MENU_RIGHT = 14;
constexpr int32_t REQ_PAD = 16, BUTTON_H = 26, BUTTON_GAP = 12;

static struct GfxPoint pts[64];
static uint8_t verbs[64];

static struct GfxPath *path(void)
{
    static struct GfxPath p;

    p = (struct GfxPath){ pts, verbs, 64, 64, 0, 0 };
    return &p;
}

static void point(struct GfxPath *p, uint8_t verb, int32_t x, int32_t y)
{
    p->gp_Verbs[p->gp_NumVerbs++] = verb;
    p->gp_Points[p->gp_NumPoints++] = (struct GfxPoint){ x, y };
}

static uint64_t length(const char *s, uint64_t max)
{
    uint64_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

static void gradient(const struct GfxIFace *g, struct RastPort *rp, struct GfxRect r, uint32_t from,
                     uint32_t to, const struct GfxPath *p)
{
    struct GfxGradient gg = { r, from, to, 0 };

    g->FillGradient(rp, &gg, p, 0);
}

static void rect(const struct GfxIFace *g, struct RastPort *rp, uint32_t pen, int32_t x0, int32_t y0,
                 int32_t x1, int32_t y1)
{
    rp->rp_FgPen = pen;
    g->RectFill(rp, x0, y0, x1, y1);
}

/* A rounded rectangle filled with a gradient, its outline 'edge' a pixel
 * wide (none if 0). */
static void rounded(const struct GfxIFace *g, struct RastPort *rp, struct GfxRect r, int32_t radius,
                    uint32_t from, uint32_t to, uint32_t edge)
{
    struct GfxPath *p = path();

    if (edge) {
        g->PathRoundRect(p, &r, radius * 64);
        rp->rp_FgPen = edge;
        g->FillPath(rp, p, 0);
        p = path();
        r = (struct GfxRect){ r.MinX + 1, r.MinY + 1, r.MaxX - 1, r.MaxY - 1 };
        radius = radius > 1 ? radius - 1 : 0;
    }
    g->PathRoundRect(p, &r, radius * 64);
    gradient(g, rp, r, from, to, p);
}

/* --- The screen bar. --- */

void bar_clock(char out[12], uint64_t s)
{
    uint32_t m = (uint32_t)(s / 60 % (24 * 60)), h = m / 60 % 12;
    unsigned n = 0;

    h = h ? h : 12;
    if (h >= 10)
        out[n++] = (char)('0' + h / 10);
    out[n++] = (char)('0' + h % 10);
    out[n++] = ':';
    out[n++] = (char)('0' + m % 60 / 10);
    out[n++] = (char)('0' + m % 10);
    out[n++] = ' ';
    out[n++] = m >= 12 * 60 ? 'P' : 'A';
    out[n++] = 'M';
    out[n] = 0;
}

void bar_layout(const struct GfxIFace *g, struct bar *b)
{
    const struct TextFont *bold = b->fonts ? b->fonts->bold : nullptr;
    const struct TextFont *sans = b->fonts ? b->fonts->sans : nullptr;
    int32_t x = NAME_X + chrome_width(g, bold, "PlanarOS", 8);

    b->n = 1;
    b->x0[0] = 4;
    b->x1[0] = x + TITLE_PAD / 2;
    x += TITLE_PAD + 6;
    for (uint32_t i = 0; b->strip && i < b->strip->ms_Menus && i < INTUI_MENU_TITLES; i++) {
        const char *t = b->strip->ms_Title[i];
        int32_t w = chrome_width(g, sans, t, length(t, INTUI_LABEL - 1));

        b->x0[b->n] = x;
        b->x1[b->n] = x + w + 2 * TITLE_PAD - 1;
        x = b->x1[b->n++] + 1;
    }
}

int bar_hit(const struct bar *b, int32_t x, int32_t y)
{
    if (y < 0 || y >= BAR_H)
        return -1;
    for (unsigned i = 0; i < b->n; i++)
        if (x >= b->x0[i] && x <= b->x1[i])
            return (int)i;
    return -1;
}

/* The ring of the Planar mark, or the clock's face: a circle of 'd'
 * pixels at (x, y) with a hole 'hole' across. */
static struct GfxPath *ring(const struct GfxIFace *g, int32_t x, int32_t y, int32_t d, int32_t hole)
{
    struct GfxPath *p = path();
    int32_t in = (d - hole) / 2;

    g->PathRoundRect(p, &(struct GfxRect){ x, y, x + d - 1, y + d - 1 }, d * 32);
    g->PathRoundRect(p, &(struct GfxRect){ x + in, y + in, x + in + hole - 1, y + in + hole - 1 }, hole * 32);
    return p;
}

void bar_draw(const struct GfxIFace *g, struct BitMap *bm, const struct bar *b)
{
    const struct TextFont *bold = b->fonts ? b->fonts->bold : nullptr;
    const struct TextFont *sans = b->fonts ? b->fonts->sans : nullptr;
    int32_t w = (int32_t)b->width, ty = (BAR_H - chrome_line(sans)) / 2;
    int32_t by = (BAR_H - chrome_line(bold)) / 2;
    struct RastPort rp;
    struct GfxPath *p;

    g->InitRastPort(&rp, bm);
    gradient(g, &rp, (struct GfxRect){ 0, 1, w - 1, BAR_H - 2 }, BAR_TOP, BAR_BOTTOM, nullptr);
    rect(g, &rp, BAR_EDGE_LIGHT, 0, 0, w - 1, 0);
    rect(g, &rp, BAR_EDGE_DARK, 0, BAR_H - 1, w - 1, BAR_H - 1);

    /* An open title: a cobalt pill behind it. */
    if (b->open >= 0 && (unsigned)b->open < b->n)
        rounded(g, &rp, (struct GfxRect){ b->x0[b->open], 3, b->x1[b->open], BAR_H - 5 }, 5, HOT_TOP,
                HOT_BOTTOM, 0);

    /* The mark, a ring, and the system's name. */
    p = ring(g, MARK_X, (BAR_H - MARK_SIZE) / 2, MARK_SIZE, 8);
    rp.rp_Clip = (struct GfxRect){ 0, 0, w - 1, BAR_H - 1 };
    {
        struct GfxGradient gg = { { MARK_X, (BAR_H - MARK_SIZE) / 2, MARK_X + MARK_SIZE - 1,
                                    (BAR_H + MARK_SIZE) / 2 - 1 }, MARK_TOP, MARK_BOTTOM, 0 };

        g->FillGradient(&rp, &gg, p, PATHF_EVENODD);
    }
    chrome_text(g, &rp, bold, NAME_X, by, "PlanarOS", 8, b->open == 0 ? PEN_TEXT_LIGHT : PEN_TEXT_DARK,
                b->open == 0 ? 0 : BAR_ETCH);

    for (unsigned i = 1; i < b->n; i++) {
        const char *t = b->strip->ms_Title[i - 1];
        bool off = b->strip->ms_Disabled & 1u << (i - 1), open = b->open == (int)i;

        chrome_text(g, &rp, sans, b->x0[i] + TITLE_PAD, ty, t, length(t, INTUI_LABEL - 1),
                    open ? PEN_TEXT_LIGHT : off ? PEN_TEXT_MUTED : PEN_TEXT_DARK, open ? 0 : BAR_ETCH);
    }

    /* The clock: a face with hands, and the time. */
    if (b->clock[0]) {
        int32_t tw = chrome_width(g, sans, b->clock, length(b->clock, sizeof b->clock));
        int32_t tx = w - 12 - tw, cx = tx - 22, cy = (BAR_H - 14) / 2;

        chrome_text(g, &rp, sans, tx, ty, b->clock, length(b->clock, sizeof b->clock), PEN_TEXT_DARK,
                    BAR_ETCH);
        p = ring(g, cx, cy, 14, 10);
        rp.rp_FgPen = PEN_TEXT_DARK;
        g->FillPath(&rp, p, PATHF_EVENODD);
        rect(g, &rp, PEN_TEXT_DARK, cx + 6, cy + 3, cx + 7, cy + 7);
        rect(g, &rp, PEN_TEXT_DARK, cx + 7, cy + 6, cx + 10, cy + 7);
    }
}

/* --- A menu. --- */

static const struct IntuiMenuItem *item_of(const struct menu *m, int i)
{
    return &m->strip->ms_Item[m->first + (unsigned)i];
}

void menu_layout(const struct GfxIFace *g, const struct chrome_fonts *f, struct menu *m)
{
    const struct TextFont *sans = f ? f->sans : nullptr;
    int32_t w = 0, y = MENU_PAD;
    bool keys = false;

    m->first = m->count = 0;
    for (unsigned i = 0; i < m->strip->ms_Items && i < INTUI_MENU_ITEMS; i++)
        if (m->strip->ms_Item[i].mi_Menu == m->menu) {
            if (!m->count)
                m->first = i;
            m->count++;
        } else if (m->count) {
            break;
        }
    for (unsigned i = 0; i < m->count; i++) {
        const struct IntuiMenuItem *mi = item_of(m, (int)i);
        int32_t lw = chrome_width(g, sans, mi->mi_Label, length(mi->mi_Label, INTUI_LABEL - 1));

        w = lw > w ? lw : w;
        keys |= mi->mi_Key != 0;
        m->y[i] = y;
        y += mi->mi_Flags & ITEMBAR ? BAR_ITEM_H : ITEM_H;
    }
    m->y[m->count] = y;
    w += CHECK_W + MENU_RIGHT + (keys ? KEY_W : 0);
    m->w = (uint32_t)(w < 120 ? 120 : w);
    m->h = (uint32_t)(y + MENU_PAD);
}

int menu_hit(const struct menu *m, int32_t x, int32_t y)
{
    if (x < 0 || x >= (int32_t)m->w)
        return -1;
    for (unsigned i = 0; i < m->count; i++)
        if (y >= m->y[i] && y < m->y[i + 1])
            return (int)i;
    return -1;
}

bool menu_choosable(const struct menu *m, int item)
{
    return item >= 0 && (unsigned)item < m->count && !(m->strip->ms_Disabled & 1u << m->menu) &&
           !(item_of(m, item)->mi_Flags & (ITEMBAR | NM_ITEMDISABLED));
}

/* A check mark, its top left at (x, y), 12 by 10. */
static void check(const struct GfxIFace *g, struct RastPort *rp, int32_t x, int32_t y, uint32_t pen)
{
    static const int8_t at[][2] = { { 0, 5 }, { 2, 3 }, { 4, 6 }, { 10, 0 }, { 12, 2 }, { 4, 10 } };
    struct GfxPath *p = path();

    for (unsigned i = 0; i < 6; i++)
        point(p, i ? PATH_LINE : PATH_MOVE, (x + at[i][0]) * 64, (y + at[i][1]) * 64);
    rp->rp_FgPen = pen;
    g->FillPath(rp, p, 0);
}

/* The GUI key's mark: a diamond 9 across, centred at (x, y). */
static void diamond(const struct GfxIFace *g, struct RastPort *rp, int32_t x, int32_t y, uint32_t pen)
{
    struct GfxPath *p = path();
    int32_t cx = x * 64 + 32, cy = y * 64 + 32, r = 4 * 64 + 32;

    point(p, PATH_MOVE, cx, cy - r);
    point(p, PATH_LINE, cx + r, cy);
    point(p, PATH_LINE, cx, cy + r);
    point(p, PATH_LINE, cx - r, cy);
    rp->rp_FgPen = pen;
    g->FillPath(rp, p, 0);
}

void menu_draw(const struct GfxIFace *g, const struct chrome_fonts *f, struct BitMap *bm,
               const struct menu *m)
{
    const struct TextFont *sans = f ? f->sans : nullptr;
    int32_t w = (int32_t)m->w, h = (int32_t)m->h, ty = (ITEM_H - chrome_line(sans)) / 2;
    struct RastPort rp;

    g->InitRastPort(&rp, bm);
    gradient(g, &rp, (struct GfxRect){ 0, 0, w - 1, h - 1 }, PANE_TOP, PANE_BOTTOM, nullptr);
    rect(g, &rp, PANE_BORDER, 0, 0, w - 1, 0);
    rect(g, &rp, PANE_BORDER, 0, h - 1, w - 1, h - 1);
    rect(g, &rp, PANE_BORDER, 0, 0, 0, h - 1);
    rect(g, &rp, PANE_BORDER, w - 1, 0, w - 1, h - 1);
    rect(g, &rp, BAR_EDGE_LIGHT, 1, 1, w - 2, 1);
    rect(g, &rp, BAR_EDGE_LIGHT, 1, 1, 1, h - 2);
    for (unsigned i = 0; i < m->count; i++) {
        const struct IntuiMenuItem *mi = item_of(m, (int)i);
        int32_t y = m->y[i];
        bool hot = m->hot == (int)i && menu_choosable(m, (int)i);
        uint32_t pen = hot ? PEN_TEXT_LIGHT : menu_choosable(m, (int)i) ? PANE_TEXT : PEN_TEXT_MUTED;

        if (mi->mi_Flags & ITEMBAR) {           /* a groove */
            rect(g, &rp, PANE_GROOVE_DARK, 6, y + 3, w - 7, y + 3);
            rect(g, &rp, BAR_EDGE_LIGHT, 6, y + 4, w - 7, y + 4);
            continue;
        }
        if (hot)
            rounded(g, &rp, (struct GfxRect){ 3, y, w - 4, y + ITEM_H - 1 }, 4, HOT_TOP, HOT_BOTTOM, 0);
        if (mi->mi_Flags & CHECKIT && mi->mi_Flags & CHECKED)
            check(g, &rp, 8, y + (ITEM_H - 10) / 2, pen);
        chrome_text(g, &rp, sans, CHECK_W, y + ty, mi->mi_Label, length(mi->mi_Label, INTUI_LABEL - 1),
                    pen, 0);
        if (mi->mi_Key) {
            int32_t kx = w - MENU_RIGHT - chrome_width(g, sans, &mi->mi_Key, 1);

            diamond(g, &rp, kx - 9, y + ITEM_H / 2 - 1, pen);
            chrome_text(g, &rp, sans, kx, y + ty, &mi->mi_Key, 1, pen, 0);
        }
    }
}

/* --- A requester's inside. --- */

bool request_layout(const struct GfxIFace *g, const struct chrome_fonts *f, struct request *q,
                    uint32_t max_w)
{
    const struct TextFont *sans = f ? f->sans : nullptr;
    int32_t line = chrome_line(sans) + 3, w = 0, bw = 0, x;
    const char *s;

    q->r.ir_Title[INTUI_TITLE - 1] = q->r.ir_Body[INTUI_REQ_BODY - 1] = 0;
    q->r.ir_Gadgets[INTUI_REQ_GADGETS - 1] = 0;
    q->nlines = q->nbuttons = 0;
    q->pressed = -1;
    for (s = q->r.ir_Body; q->nlines < REQ_LINES;) {
        uint32_t n = 0;

        while (s[n] && s[n] != '\n')
            n++;
        q->line[q->nlines] = s;
        q->len[q->nlines++] = n;
        if (!s[n])
            break;
        s += n + 1;
    }
    for (s = q->r.ir_Gadgets; *s && q->nbuttons < REQ_BUTTONS;) {
        uint32_t n = 0;

        while (s[n] && s[n] != '|')
            n++;
        q->label[q->nbuttons] = s;
        q->llen[q->nbuttons++] = n;
        if (!s[n])
            break;
        s += n + 1;
    }
    if (!q->nbuttons)
        return false;
    for (unsigned i = 0; i < q->nlines; i++) {
        int32_t lw = chrome_width(g, sans, q->line[i], q->len[i]);

        w = lw > w ? lw : w;
    }
    for (unsigned i = 0; i < q->nbuttons; i++) {
        int32_t lw = chrome_width(g, sans, q->label[i], q->llen[i]) + 28;

        bw += (lw < 72 ? 72 : lw) + (i ? BUTTON_GAP : 0);
    }
    w = (w > bw ? w : bw) + 2 * REQ_PAD;
    w = w < 240 ? 240 : w > (int32_t)max_w ? (int32_t)max_w : w;
    q->w = (uint32_t)w;
    q->h = (uint32_t)(REQ_PAD + (int32_t)q->nlines * line + REQ_PAD + BUTTON_H + REQ_PAD);
    /* The buttons: the first at the left, the last at the right, the rest
     * spread between; one alone in the middle. */
    x = REQ_PAD;
    for (unsigned i = 0; i < q->nbuttons; i++) {
        int32_t lw = chrome_width(g, sans, q->label[i], q->llen[i]) + 28;
        int32_t y = (int32_t)q->h - REQ_PAD - BUTTON_H;

        lw = lw < 72 ? 72 : lw;
        if (q->nbuttons == 1)
            x = (w - lw) / 2;               /* alone: in the middle */
        else if (i && i + 1 == q->nbuttons)
            x = w - REQ_PAD - lw;
        else if (i)
            x = REQ_PAD + (w - 2 * REQ_PAD - lw) * (int32_t)i / (int32_t)(q->nbuttons - 1);
        q->button[i] = (struct GfxRect){ x, y, x + lw - 1, y + BUTTON_H - 1 };
    }
    return true;
}

void request_draw(const struct GfxIFace *g, const struct chrome_fonts *f, struct BitMap *bm,
                  const struct request *q)
{
    const struct TextFont *sans = f ? f->sans : nullptr;
    int32_t line = chrome_line(sans) + 3, ty = (BUTTON_H - chrome_line(sans)) / 2;
    struct RastPort rp;

    g->InitRastPort(&rp, bm);
    gradient(g, &rp, (struct GfxRect){ 0, 0, (int32_t)q->w - 1, (int32_t)q->h - 1 }, PANE_TOP, PANE_BOTTOM,
             nullptr);
    for (unsigned i = 0; i < q->nlines; i++)
        chrome_text(g, &rp, sans, REQ_PAD, REQ_PAD + (int32_t)i * line, q->line[i], q->len[i], PANE_TEXT,
                    BAR_ETCH);
    for (unsigned i = 0; i < q->nbuttons; i++) {
        struct GfxRect r = q->button[i];
        bool down = q->pressed == (int)i;
        int32_t lw = chrome_width(g, sans, q->label[i], q->llen[i]);

        rounded(g, &rp, r, 6, down ? AMBER_TOP : THUMB_TOP, down ? AMBER_BOTTOM : THUMB_BOTTOM, PANE_BORDER);
        chrome_text(g, &rp, sans, r.MinX + (r.MaxX - r.MinX + 1 - lw) / 2, r.MinY + ty, q->label[i],
                    q->llen[i], PEN_TEXT_DARK, 0);
    }
}

int request_hit(const struct request *q, int32_t x, int32_t y)
{
    for (unsigned i = 0; i < q->nbuttons; i++)
        if (x >= q->button[i].MinX && x <= q->button[i].MaxX && y >= q->button[i].MinY &&
            y <= q->button[i].MaxY)
            return (int)i;
    return -1;
}

int request_answer(const struct request *q, int button)
{
    return button + 1 == (int)q->nbuttons ? 0 : button + 1;
}
