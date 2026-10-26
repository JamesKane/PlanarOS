/*
 * lib/ui/test.c -- ui.library on the host (make ui-test)
 *
 * lib/ui alone, with the sanitizers, drawn by lib/gfx's scalar entries
 * and NEON's on an Arm host, in System Sans and Mono from lib/font linked
 * in. First the model: what AddGadget refuses, a layout's rectangles
 * (inside their group, apart from each other, the weighted taking the
 * room left, again at a new size), a text field's editing, a list's
 * scrolling, a slider's keys, the split held where both sides fit, Tab.
 * Then every gadget in each of its states drawn on both panes, against
 * a CRC (states.ppm, looked at once). Then the window test's gadgets
 * (conformance/uiwin.h) clicked through with made-up events, each
 * step's change as uiwin.h says; after each, what was drawn again must
 * be what a whole drawing gives, and every pixel that changed inside
 * the damage. Its CRCs, first and last, are the ones window.c reads
 * back on QEMU. The pictures are in build/ui.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/font.h>

#include "../../conformance/uiwin.h"
#include "../gfx/rows.h"

extern const struct FontIFace *gfx_host_ifont;

/* Areas: the host's memory, page aligned and zeroed (the fonts' caches). */
static void *areas[32];

int64_t AllocArea(uint64_t size, uint32_t, void **address)
{
    size = (size + 4095) & ~4095ull;
    for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++)
        if (!areas[i]) {
            if (!(areas[i] = aligned_alloc(4096, size)))
                return ERR_NOMEM;
            memset(areas[i], 0, size);
            *address = areas[i];
            return i + 1;
        }
    return ERR_NOMEM;
}

int64_t FreeArea(Handle h)
{
    if (h < 1 || h > sizeof areas / sizeof areas[0] || !areas[h - 1])
        return ERR_BADHANDLE;
    free(areas[h - 1]);
    areas[h - 1] = nullptr;
    return 0;
}

int64_t QueryArea(const void *, struct AreaInfo *)
{
    return ERR_NOTFOUND;
}

#define TABLE(fill, blt, text, path, mask, fpath, grad, run) { \
    .AllocBitMap = AllocBitMap, .FreeBitMap = FreeBitMap, .InitBitMap = InitBitMap, \
    .InitRastPort = InitRastPort, .RectFill = fill, .BltBitMap = blt, .DrawBevel = DrawBevel, \
    .Text = text, .PathBounds = PathBounds, .PathMaskScratch = PathMaskScratch, .PathMask = path, \
    .BltMask = mask, .FillPath = fpath, .FillGradient = grad, .PathRoundRect = PathRoundRect, \
    .InitTmpRas = InitTmpRas, .OpenTextFont = OpenTextFont, .CloseTextFont = CloseTextFont, \
    .SetFont = SetFont, .TextLength = TextLength, .TextRun = run }

static const struct GfxIFace scalar =
    TABLE(RectFill, BltBitMap, Text, PathMask, BltMask, FillPath, FillGradient, TextRun);

#ifdef GFX_TEST_NEON
static const struct GfxIFace neon = TABLE(RectFillVector, BltBitMapVector, TextVector, PathMaskVector,
                                          BltMaskVector, FillPathVector, FillGradientVector, TextRunVector);
#endif

static const struct UIIFace iui = {
    .InitUI = InitUI, .FreeUI = FreeUI, .AddGadget = AddGadget, .SetGadgetAttrsA = SetGadgetAttrsA,
    .LayoutUI = LayoutUI, .DrawUI = DrawUI, .HandleUIEvent = HandleUIEvent,
};

/* Every gadget in each state, on both panes: looked at once (states.ppm). */
constexpr uint32_t STATES_CRC = 0x7cba58bb;

static struct UI ui;
static unsigned failures;

#define CHECK(cond) \
    do \
        if (!(cond)) { \
            printf("ui-test: FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    while (0)

static uint32_t crc_of(const struct BitMap *bm)
{
    uint32_t crc = ~0u;

    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            uint32_t px = ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x] &
                          0xffffff;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

static uint32_t px(const struct BitMap *bm, uint32_t x, uint32_t y)
{
    return ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x];
}

static void picture(const char *name, const struct BitMap *bm)
{
    char path[64];
    FILE *f;

    snprintf(path, sizeof path, "build/ui/%s.ppm", name);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n%u %u\n255\n", bm->bm_Width, bm->bm_Height);
    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            uint32_t p = px(bm, x, y);
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };

            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static uint64_t now;

static uint32_t event(uint32_t cls, uint16_t code, uint32_t ch, int32_t x, int32_t y, struct UIChange *c)
{
    struct IntuiEvent ev = { now, cls, 1, code, 0, ch, x, y };

    now += 20000000;
    return HandleUIEvent(&ui, &ev, c);
}

static uint32_t click(int32_t x, int32_t y, struct UIChange *last)
{
    struct UIChange c;
    uint32_t got = 0;

    *last = (struct UIChange){};
    if (event(IDCMP_MOUSEBUTTONS, SELECTDOWN, 0, x, y, &c))
        *last = c, got = c.uc_Gadget;
    if (event(IDCMP_MOUSEBUTTONS, SELECTUP, 0, x, y, &c))
        *last = c, got = c.uc_Gadget;
    return got;
}

static uint32_t typed(uint32_t ch, struct UIChange *c)
{
    return event(IDCMP_VANILLAKEY, (uint16_t)ch, ch, 0, 0, c);
}

static uint32_t raw(uint16_t usage, struct UIChange *c)
{
    return event(IDCMP_RAWKEY, usage, 0, 0, 0, c);
}

static bool within_rect(struct GfxRect r, int32_t x, int32_t y)
{
    return x >= r.MinX && x <= r.MaxX && y >= r.MinY && y <= r.MaxY;
}

static bool inside(struct GfxRect in, struct GfxRect r)
{
    return r.MinX >= in.MinX && r.MinY >= in.MinY && r.MaxX <= in.MaxX && r.MaxY <= in.MaxY;
}

static bool apart(struct GfxRect a, struct GfxRect b)
{
    return a.MaxX < b.MinX || b.MaxX < a.MinX || a.MaxY < b.MinY || b.MaxY < a.MinY;
}

/* Every group's children inside it and apart from each other. */
static bool laid_out(uint32_t n)
{
    const struct UIGadget *g = &ui.ui_Gadget[n];

    for (uint32_t c = g->g_Child; c; c = ui.ui_Gadget[c].g_Next) {
        if (!inside(g->g_Rect, ui.ui_Gadget[c].g_Rect) || !laid_out(c))
            return false;
        for (uint32_t d = ui.ui_Gadget[c].g_Next; d; d = ui.ui_Gadget[d].g_Next)
            if (!apart(ui.ui_Gadget[c].g_Rect, ui.ui_Gadget[d].g_Rect))
                return false;
    }
    return true;
}

/* --- The model. --- */

static char field[UW_FIELD];

static void model(void)
{
    struct UIChange c;
    struct UIGadget *g;
    int32_t h;

    /* Refusals. */
    CHECK(InitUI(&ui, &scalar) == 0);
    CHECK(AddGadgetTags(&iui, &ui, 1, UI_ROW, { UIA_Margin, 0 }) == 0);       /* no root yet */
    CHECK(AddGadgetTags(&iui, &ui, 0, 99, { UIA_Margin, 0 }) == 0);
    CHECK(AddGadgetTags(&iui, &ui, 0, UI_SPLITPANE, { UIA_Value, 0 }) == 1);
    CHECK(AddGadgetTags(&iui, &ui, 0, UI_ROW, { UIA_Margin, 0 }) == 0);       /* a second root */
    CHECK(AddGadgetTags(&iui, &ui, 1, UI_LABEL, { UIA_Label, (uint64_t)"a" }) == 2);
    CHECK(AddGadgetTags(&iui, &ui, 2, UI_LABEL, { UIA_Label, (uint64_t)"b" }) == 0);   /* a label's no group */
    CHECK(AddGadgetTags(&iui, &ui, 1, UI_COLUMN, { UIA_Margin, 0 }) == 3);
    CHECK(AddGadgetTags(&iui, &ui, 1, UI_COLUMN, { UIA_Margin, 0 }) == 0);    /* a split takes two */
    CHECK(AddGadgetTags(&iui, &ui, 9, UI_LABEL, { UIA_Label, (uint64_t)"c" }) == 0);   /* no such parent */
    CHECK(ui.ui_Gadget[2].g_Pane == UIPANE_DARK && ui.ui_Gadget[3].g_Pane == UIPANE_LIGHT);
    while (ui.ui_Count < UI_GADGETS)
        CHECK(AddGadgetTags(&iui, &ui, 3, UI_LABEL, { UIA_Label, (uint64_t)"x" }) != 0);
    CHECK(AddGadgetTags(&iui, &ui, 3, UI_LABEL, { UIA_Label, (uint64_t)"y" }) == 0);   /* full */
    FreeUI(&ui);

    /* The window test's layout, at its size and others. */
    CHECK(InitUI(&ui, &scalar) == 0 && uw_build(&iui, &ui, field));
    LayoutUI(&ui, UW_WIDTH, UW_HEIGHT);
    CHECK(laid_out(1));
    CHECK(ui.ui_Gadget[UW_SPLIT].g_Value == 240);
    CHECK(ui.ui_Gadget[UW_LEFTCOL].g_Rect.MaxX == 239 && ui.ui_Gadget[UW_RIGHTCOL].g_Rect.MinX == 242);
    CHECK(ui.ui_Gadget[UW_NAME].g_Rect.MaxX == (int32_t)UW_WIDTH - 1 - 14);        /* a column's filled */
    CHECK(ui.ui_Gadget[UW_SAVE].g_Rect.MaxX == (int32_t)UW_WIDTH - 1 - 14);        /* the buttons' row at its end */
    CHECK(ui.ui_Gadget[UW_BADGE].g_Rect.MaxX == 239 - 14);
    CHECK(ui.ui_Gadget[UW_BUTTONS].g_Rect.MaxY == (int32_t)UW_HEIGHT - 1 - 14);    /* the list took the room */
    h = ui.ui_Gadget[UW_FILES].g_Rect.MaxY - ui.ui_Gadget[UW_FILES].g_Rect.MinY;
    LayoutUI(&ui, UW_WIDTH + 100, UW_HEIGHT + 50);
    CHECK(laid_out(1));
    CHECK(ui.ui_Gadget[UW_FILES].g_Rect.MaxY - ui.ui_Gadget[UW_FILES].g_Rect.MinY == h + 50);
    CHECK(ui.ui_Gadget[UW_SAVE].g_Rect.MaxX == (int32_t)UW_WIDTH + 99 - 14);
    LayoutUI(&ui, 300, 100);                            /* too small: the split's left wins, and nothing breaks */
    CHECK(ui.ui_Gadget[UW_LEFTCOL].g_Rect.MaxX + 1 == ui.ui_Gadget[UW_LEFTCOL].g_NatW);
    LayoutUI(&ui, UW_WIDTH, UW_HEIGHT);                 /* and the split back where it was asked to be */
    CHECK(ui.ui_Gadget[UW_LEFTCOL].g_Rect.MaxX == 239);

    /* Nothing reported without a rastport but the changes; damage kept. */
    g = &ui.ui_Gadget[UW_NAME];
    CHECK(click(g->g_Rect.MinX + 10, g->g_Rect.MinY + 12, &c) == 0 && ui.ui_Focus == UW_NAME);
    typed('h', &c), typed(0xe9, &c), typed('l', &c), typed('l', &c), typed('o', &c);
    CHECK(!strcmp(field, "h\xc3\xa9llo") && g->g_Cursor == 6);
    raw(0x50, &c), raw(0x50, &c), raw(0x50, &c), raw(0x50, &c);     /* left, by characters */
    CHECK(g->g_Cursor == 1);
    typed(0x7f, &c);                                                 /* the é, forward */
    CHECK(!strcmp(field, "hllo") && g->g_Cursor == 1);
    typed('\b', &c);
    CHECK(!strcmp(field, "llo") && g->g_Cursor == 0);
    typed('\b', &c);
    CHECK(!strcmp(field, "llo"));
    raw(0x4d, &c);                                                   /* End */
    for (unsigned i = 0; i < 40; i++)
        typed('x', &c);
    CHECK(strlen(field) == UW_FIELD - 1 && g->g_Cursor == (int32_t)UW_FIELD - 1);   /* full: the rest dropped */
    LayoutUI(&ui, 420, UW_HEIGHT);
    CHECK(g->g_Top > 0);                                             /* narrower: scrolled to the cursor */
    LayoutUI(&ui, UW_WIDTH, UW_HEIGHT);
    CHECK(typed('\n', &c) == UW_NAME && c.uc_Value == (int32_t)UW_FIELD - 1 && c.uc_Flags == UICF_ENTER);
    typed(0x15, &c);
    CHECK(field[0] == 0 && g->g_Cursor == 0 && g->g_Top == 0);

    /* A list's keys: the selection kept in sight, five rows of it shown. */
    LayoutUI(&ui, UW_WIDTH, 300);
    g = &ui.ui_Gadget[UW_FILES];
    CHECK(click(g->g_Rect.MinX + 20, g->g_Rect.MinY + UW_ROW(0), &c) == UW_FILES && c.uc_Value == 0);
    CHECK(raw(0x4d, &c) == UW_FILES && c.uc_Value == 7 && g->g_Top > 0);     /* End */
    CHECK(raw(0x51, &c) == 0);                                               /* past the end: nothing */
    CHECK(raw(0x4a, &c) == UW_FILES && c.uc_Value == 0 && g->g_Top == 0);    /* Home */
    CHECK(typed('\n', &c) == UW_FILES && c.uc_Flags == UICF_DOUBLE);
    LayoutUI(&ui, UW_WIDTH, UW_HEIGHT);

    /* A slider's keys, Shift a tenth; the segmented's. */
    g = &ui.ui_Gadget[UW_SPEED];
    CHECK(click(g->g_Rect.MaxX - 4, g->g_Rect.MinY + 11, &c) == UW_SPEED && c.uc_Value == 20);
    CHECK(raw(0x4f, &c) == 0);                                               /* at its end */
    CHECK(raw(0x4a, &c) == UW_SPEED && c.uc_Value == 0);
    {
        struct IntuiEvent ev = { now, IDCMP_RAWKEY, 1, 0x52, 0x02, 0, 0, 0 };

        CHECK(HandleUIEvent(&ui, &ev, &c) == UW_SPEED && c.uc_Value == 2);    /* Shift-Up */
    }

    /* Tab: round the ones that take focus, the disabled toggle passed over. */
    ui.ui_Focus = UW_MIXER;
    typed('\t', &c);
    CHECK(ui.ui_Focus == UW_SPEED);
    typed('\t', &c), typed('\t', &c), typed('\t', &c), typed('\t', &c), typed('\t', &c);
    CHECK(ui.ui_Focus == UW_SAVE);
    typed('\t', &c);
    CHECK(ui.ui_Focus == UW_REPEAT);
    {
        struct IntuiEvent ev = { now, IDCMP_VANILLAKEY, 1, '\t', 0x20, '\t', 0, 0 };

        HandleUIEvent(&ui, &ev, &c);
        CHECK(ui.ui_Focus == UW_SAVE);                                       /* Shift-Tab, back round */
    }

    /* The split dragged by its groove, held where both sides fit. */
    g = &ui.ui_Gadget[UW_SPLIT];
    CHECK(event(IDCMP_MOUSEBUTTONS, SELECTDOWN, 0, 240, 100, &c) == 0 && ui.ui_Pressed == UW_SPLIT);
    CHECK(event(IDCMP_MOUSEMOVE, 0, 0, 280, 100, &c) == UW_SPLIT && c.uc_Value == 280 && c.uc_Flags == UICF_DRAG);
    CHECK(ui.ui_Gadget[UW_RIGHTCOL].g_Rect.MinX == 282 && laid_out(1));
    event(IDCMP_MOUSEMOVE, 0, 0, 0, 100, &c);
    CHECK(g->g_Value == ui.ui_Gadget[UW_LEFTCOL].g_NatW);
    event(IDCMP_MOUSEMOVE, 0, 0, 1000, 100, &c);
    CHECK(g->g_Value == (int32_t)UW_WIDTH - 2 - ui.ui_Gadget[UW_RIGHTCOL].g_NatW);
    CHECK(event(IDCMP_MOUSEBUTTONS, SELECTUP, 0, 1000, 100, &c) == UW_SPLIT && c.uc_Flags == 0);

    /* A button let go of off it: nothing. */
    g = &ui.ui_Gadget[UW_SAVE];
    LayoutUI(&ui, UW_WIDTH, UW_HEIGHT);
    event(IDCMP_MOUSEBUTTONS, SELECTDOWN, 0, g->g_Rect.MinX + 10, g->g_Rect.MinY + 10, &c);
    CHECK(ui.ui_Inside);
    event(IDCMP_MOUSEMOVE, 0, 0, 5, 5, &c);
    CHECK(!ui.ui_Inside);
    CHECK(event(IDCMP_MOUSEBUTTONS, SELECTUP, 0, 5, 5, &c) == 0);
    FreeUI(&ui);
}

/* --- Every gadget in each state. --- */

static uint32_t states(const struct GfxIFace *gfx, bool keep)
{
    static const char *const three[] = { "One", "Two", "Three" };
    static char text[16] = "Edited";
    struct BitMap *bm = gfx->AllocBitMap(560, 440, PIXFMT_XRGB8888, 0, nullptr);
    struct RastPort rp;
    uint32_t side[2], crc;

    if (!bm)
        return 0;
    gfx->InitRastPort(&rp, bm);
    InitUI(&ui, gfx);
    AddGadgetTags(&iui, &ui, 0, UI_SPLITPANE, { UIA_Value, 280 });
    for (unsigned s = 0; s < 2; s++) {
        uint32_t c = side[s] = AddGadgetTags(&iui, &ui, 1, UI_COLUMN, { UIA_Margin, 10 }, { UIA_Spacing, 6 });

        AddGadgetTags(&iui, &ui, c, UI_TOGGLE, { UIA_Label, (uint64_t)"Cobalt off" });
        AddGadgetTags(&iui, &ui, c, UI_TOGGLE, { UIA_Label, (uint64_t)"Cobalt on" }, { UIA_Value, 1 });
        AddGadgetTags(&iui, &ui, c, UI_TOGGLE, { UIA_Label, (uint64_t)"Amber off" }, { UIA_Variant, TOGGLE_AMBER },
                      { UIA_Glyph, (uint64_t)"%" });
        AddGadgetTags(&iui, &ui, c, UI_TOGGLE, { UIA_Label, (uint64_t)"Amber on" }, { UIA_Variant, TOGGLE_AMBER },
                      { UIA_Value, 1 }, { UIA_Glyph, (uint64_t)"2" });
        AddGadgetTags(&iui, &ui, c, UI_TOGGLE, { UIA_Label, (uint64_t)"Disabled" }, { UIA_Disabled, 1 });
        AddGadgetTags(&iui, &ui, c, UI_DIVIDER, { UIA_Height, 4 });
        AddGadgetTags(&iui, &ui, c, UI_SLIDER, { UIA_Label, (uint64_t)"Slider" }, { UIA_Value, 30 });
        AddGadgetTags(&iui, &ui, c, UI_SLIDER, { UIA_Label, (uint64_t)"Disabled" }, { UIA_Value, 60 },
                      { UIA_Disabled, 1 });
        AddGadgetTags(&iui, &ui, c, UI_SEGMENTED, { UIA_Items, (uint64_t)three }, { UIA_Count, 3 },
                      { UIA_Value, (uint64_t)s * 2 });
        AddGadgetTags(&iui, &ui, c, UI_LIST, { UIA_Items, (uint64_t)three }, { UIA_Count, 3 },
                      { UIA_Value, (uint64_t)s }, { UIA_Height, 2 + 3 * 22 + 4 });
        AddGadgetTags(&iui, &ui, c, UI_TEXTFIELD, { UIA_Buffer, (uint64_t)text }, { UIA_Size, sizeof text });
        AddGadgetTags(&iui, &ui, c, UI_ROW, { UIA_Spacing, 6 });
        AddGadgetTags(&iui, &ui, ui.ui_Count, UI_BUTTON, { UIA_Label, (uint64_t)"Button" });
        AddGadgetTags(&iui, &ui, ui.ui_Count - 1, UI_HUDBADGE, { UIA_Label, (uint64_t)"CPU 0.8%" });
    }
    ui.ui_RPort = &rp;
    LayoutUI(&ui, 560, 440);
    /* Focus on the dark side's slider, the light side's text field; the light side's button held down. */
    ui.ui_Focus = side[0] + 7;
    ui.ui_Pressed = side[1] + 13;
    ui.ui_Inside = true;
    DrawUI(&ui);
    ui.ui_Focus = side[1] + 11;
    SetGadgetAttrs(&iui, &ui, side[1] + 11, { UIA_Value, 0 });      /* drawn again, focused */
    SetGadgetAttrs(&iui, &ui, side[0] + 7, { UIA_Value, 30 });      /* and unfocused */
    crc = crc_of(bm);
    if (keep)
        picture("states", bm);
    FreeUI(&ui);
    gfx->FreeBitMap(bm);
    return crc;
}

/* --- The window test's gadgets, clicked through. --- */

static const struct { const char *name; uint32_t cls; uint16_t code; } keys[] = {
    { "ret", IDCMP_VANILLAKEY, '\n' }, { "tab", IDCMP_VANILLAKEY, '\t' }, { "spc", IDCMP_VANILLAKEY, ' ' },
    { "left", IDCMP_RAWKEY, 0x50 }, { "right", IDCMP_RAWKEY, 0x4f }, { "down", IDCMP_RAWKEY, 0x51 },
    { "up", IDCMP_RAWKEY, 0x52 },
};

/* Every pixel that differs from 'was' inside the damage. */
static bool damaged(const struct BitMap *was, const struct BitMap *bm)
{
    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            bool in = false;

            if (px(was, x, y) == px(bm, x, y))
                continue;
            for (uint32_t i = 0; i < ui.ui_NumDamage && !in; i++)
                in = within_rect(ui.ui_Damage[i], (int32_t)x, (int32_t)y);
            if (!in)
                return false;
        }
    return true;
}

static bool window(const char *name, const struct GfxIFace *gfx, uint32_t *ready, uint32_t *done)
{
    struct BitMap *bm = gfx->AllocBitMap(UW_WIDTH, UW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr);
    struct BitMap *whole = gfx->AllocBitMap(UW_WIDTH, UW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr);
    struct BitMap *was = gfx->AllocBitMap(UW_WIDTH, UW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr);
    struct RastPort rp, wrp;
    bool ok = true, keep = !strcmp(name, "scalar");
    int32_t lx = -1, ly = -1;

    if (!bm || !whole || !was || InitUI(&ui, gfx) != 0 || !uw_build(&iui, &ui, field))
        return false;
    gfx->InitRastPort(&rp, bm);
    gfx->InitRastPort(&wrp, whole);
    ui.ui_RPort = &rp;
    LayoutUI(&ui, UW_WIDTH, UW_HEIGHT);
    DrawUI(&ui);
    *ready = crc_of(bm);
    if (keep)
        picture("ready", bm);
    now = 1000000000;
    for (unsigned i = 0; i < sizeof uw_script / sizeof uw_script[0]; i++) {
        const struct uw_step *s = &uw_script[i];
        struct UIChange last = {}, c;
        int32_t x = 0, y = 0;

        memcpy(was->bm_BaseAddr, bm->bm_BaseAddr, (size_t)bm->bm_BytesPerRow * bm->bm_Height);
        ui.ui_NumDamage = 0;
        if (s->action == UW_CLICK || s->action == UW_DRAG)
            uw_point(&ui, s, &x, &y);
        /* A second click where the last was is a double click's; else a second's gone by. */
        now += s->action == UW_CLICK && x == lx && y == ly ? 200000000 : 1000000000;
        switch (s->action) {
        case UW_CLICK:
            click(x, y, &last);
            lx = x, ly = y;
            break;
        case UW_DRAG:
            if (event(IDCMP_MOUSEBUTTONS, SELECTDOWN, 0, x, y, &c))
                last = c;
            if (event(IDCMP_MOUSEMOVE, 0, 0, x + s->drag / 2, y, &c))
                last = c;
            if (event(IDCMP_MOUSEMOVE, 0, 0, x + s->drag, y, &c))
                last = c;
            if (event(IDCMP_MOUSEBUTTONS, SELECTUP, 0, x + s->drag, y, &c))
                last = c;
            lx = -1;
            break;
        case UW_TEXT:
            for (const char *t = s->text; *t; t++)
                if (typed((uint8_t)*t, &c))
                    last = c;
            lx = -1;
            break;
        case UW_KEY:
            for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if (!strcmp(keys[k].name, s->text) && event(keys[k].cls, keys[k].code, keys[k].cls == IDCMP_VANILLAKEY
                                                            ? keys[k].code : 0, 0, 0, &c))
                    last = c;
            lx = -1;
            break;
        }
        if (last.uc_Gadget != s->want || last.uc_Value != s->value || last.uc_Flags != s->flags) {
            printf("ui-test: FAILED: %s: step %u: changed %u to %d (%x), not %u to %d (%x)\n", name, i + 1,
                   last.uc_Gadget, last.uc_Value, last.uc_Flags, s->want, s->value, s->flags);
            ok = false;
        }
        if (!damaged(was, bm)) {
            printf("ui-test: FAILED: %s: step %u: pixels changed outside the damage\n", name, i + 1);
            ok = false;
        }
        ui.ui_RPort = &wrp;
        DrawUI(&ui);
        ui.ui_RPort = &rp;
        if (crc_of(whole) != crc_of(bm)) {
            printf("ui-test: FAILED: %s: step %u: drawn again in part isn't as drawn whole\n", name, i + 1);
            if (keep) {
                picture("part", bm);
                picture("whole", whole);
            }
            ok = false;
        }
    }
    ok &= !strcmp(field, "Planar Prefs");
    *done = crc_of(bm);
    if (keep)
        picture("done", bm);
    FreeUI(&ui);
    gfx->FreeBitMap(bm);
    gfx->FreeBitMap(whole);
    gfx->FreeBitMap(was);
    return ok;
}

int main(void)
{
    static const struct FontIFace font = {
        .SystemFont = SystemFont, .OpenFont = OpenFont, .FontInfo = FontInfo, .FontName = FontName,
        .MapCodepoint = MapCodepoint, .GetGlyphMetrics = GetGlyphMetrics, .GlyphOutline = GlyphOutline,
        .ScaleUnits = ScaleUnits, .ShapeRun = ShapeRun, .FitLine = FitLine, .LineSpacing = LineSpacing,
    };
    uint32_t ready, done, st;

    gfx_host_ifont = &font;
    model();
    if (failures)
        return 1;
    printf("ui-test: ok: refusals, layout at three sizes, a text field's editing, a list's, a slider's and the "
           "segmented's keys, Tab, the split dragged, a button let go of off it\n");

    st = states(&scalar, true);
    if (st != STATES_CRC) {
        printf("ui-test: FAILED: every gadget's states are %08x, not %08x (build/ui/states.ppm)\n", st, STATES_CRC);
        return 1;
    }
    if (!window("scalar", &scalar, &ready, &done))
        return 1;
    if (ready != uw_crc_ready || done != uw_crc_done) {
        printf("ui-test: FAILED: the window test's gadgets are %08x and %08x, not uiwin.h's %08x and %08x\n", ready,
               done, uw_crc_ready, uw_crc_done);
        return 1;
    }
#ifdef GFX_TEST_NEON
    {
        uint32_t nready, ndone;

        if (states(&neon, false) != st || !window("neon", &neon, &nready, &ndone) || nready != ready ||
            ndone != done) {
            printf("ui-test: FAILED: NEON doesn't draw the gadgets as the scalar code does\n");
            return 1;
        }
    }
#endif
    printf("ui-test: ok: every gadget's states (%08x), and the window test's clicked through, %u steps, each "
           "change as uiwin.h has it, drawn in part as whole, the damage covering it (%08x, %08x)%s\n",
           st, (unsigned)(sizeof uw_script / sizeof uw_script[0]), ready, done,
#ifdef GFX_TEST_NEON
           ", scalar and NEON alike"
#else
           ""
#endif
    );
    return 0;
}
