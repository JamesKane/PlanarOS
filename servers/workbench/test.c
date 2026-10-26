/*
 * servers/workbench/test.c -- the Workbench's and Prefs' drawing on the
 * host (make wb-test) (V8 step 7)
 *
 * wb.h and prefswin.h with lib/ui and lib/gfx, scalar and NEON on an Arm
 * host, in System Sans and Mono from lib/font linked in, with the
 * sanitizers: the backdrop with QEMU's two volumes' icons, one selected,
 * a volume's window, the HUD badge on the backdrop (ui.library's
 * backfill: drawn again in part as whole), and Prefs' window, each
 * against a CRC kept here, from a picture in build/workbench looked at
 * once. Then Prefs' window worked: each slider clicked where
 * conformance/window.c clicks it gives the value it expects.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/font.h>

#include "../../lib/gfx/rows.h"
#include "../prefs/prefswin.h"
#include "wb.h"

extern const struct FontIFace *gfx_host_ifont;

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

/* QEMU's screen, less the screen bar: the backdrop window's inside. */
constexpr uint32_t BW = 800, BH = 600 - INTUI_SCREENBAR;

/* The pictures' CRCs, each looked at once. */
constexpr uint32_t BACKDROP_CRC = 0x47873d43, SELECTED_CRC = 0x666d9b82, DRAWER_CRC = 0xc57f6b66, HUD_CRC = 0x3907ca3b,
                   PREFS_CRC = 0x1845fd9a;

static unsigned failures;

#define CHECK(cond) \
    do \
        if (!(cond)) { \
            printf("wb-test: FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    while (0)

static uint32_t px(const struct BitMap *bm, uint32_t x, uint32_t y)
{
    return ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x];
}

static uint32_t crc_of(const struct BitMap *bm)
{
    uint32_t crc = ~0u;

    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            uint32_t p = px(bm, x, y) & 0xffffff;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= p >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

static void picture(const char *name, const struct BitMap *bm)
{
    char path[64];
    FILE *f;

    snprintf(path, sizeof path, "build/workbench/%s.ppm", name);
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

/* What one drawing gives, said: its CRC, against the one kept (0: none yet). */
static bool as_kept(const char *name, const struct BitMap *bm, uint32_t kept, uint32_t *got, bool keep)
{
    *got = crc_of(bm);
    if (keep)
        picture(name, bm);
    if (kept && *got != kept) {
        printf("wb-test: FAILED: %s is %08x, not %08x (build/workbench/%s.ppm)\n", name, *got, kept, name);
        return false;
    }
    return true;
}

static struct wb_icon icons[2] = { { "PLANAR", WB_SYSTEM, false, 0, 0 }, { "sd0p1", WB_STORAGE, false, 0, 0 } };

/* The backdrop and its icons, as the Workbench draws them. */
static void desktop(const struct GfxIFace *g, struct RastPort *rp, const struct TextFont *sans)
{
    wb_backdrop(g, rp, BW, BH, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM);
    wb_place(icons, 2);
    for (unsigned i = 0; i < 2; i++)
        wb_icon_draw(g, rp, sans, &icons[i]);
}

/* The HUD's backfill: the backdrop, from a copy. */
static const struct BitMap *backdrop_copy;

static void backfill(struct UI *ui, struct GfxRect r)
{
    ui->ui_Gfx->BltBitMap(backdrop_copy, r.MinX, r.MinY, ui->ui_RPort, r.MinX, r.MinY, r.MaxX - r.MinX + 1,
                          r.MaxY - r.MinY + 1, BLEND_SRC);
}

static bool run(const struct GfxIFace *g, bool keep, uint32_t crc[5])
{
    static struct UI ui;
    static struct wb_drawer d;
    struct TextFont *sans = g->OpenTextFont(&(struct TextAttr){ "System Sans", 13, 0, 0 });
    struct BitMap *bm = g->AllocBitMap(BW, BH, PIXFMT_XRGB8888, 0, nullptr);
    struct BitMap *copy = g->AllocBitMap(BW, BH, PIXFMT_XRGB8888, 0, nullptr);
    struct BitMap *win = g->AllocBitMap(WB_DRAWER_W, WB_DRAWER_H, PIXFMT_XRGB8888, 0, nullptr);
    struct BitMap *pw = g->AllocBitMap(PW_WIDTH, PW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr);
    struct InputPrefs ip = { INPUT_DELAY_DEFAULT, INPUT_RATE_DEFAULT, INPUT_SPEED_DEFAULT, INPUT_ACCEL_DEFAULT };
    struct IntuiPrefs np = { 0, INTUI_POINTER_AUTO, INTUI_BACKDROP_TOP, INTUI_BACKDROP_BOTTOM };
    struct RastPort rp, crp;
    bool ok = true;

    if (!sans || !bm || !copy || !win || !pw)
        return false;
    g->InitRastPort(&rp, bm);
    g->InitRastPort(&crp, copy);
    g->SetFont(&rp, sans);

    /* The backdrop and its icons; one selected. */
    icons[0].selected = icons[1].selected = false;
    desktop(g, &rp, sans);
    ok &= as_kept("backdrop", bm, BACKDROP_CRC, &crc[0], keep);
    icons[0].selected = true;
    desktop(g, &rp, sans);
    ok &= as_kept("selected", bm, SELECTED_CRC, &crc[1], keep);
    icons[0].selected = false;

    /* A volume's window. */
    memset(&d, 0, sizeof d);
    wb_entry(&d, &(struct DosEntry){ "EFI", 0, 1, 0, 0 });
    wb_entry(&d, &(struct DosEntry){ "startup.nsh", 41, 0, 0, 0 });
    wb_entry(&d, &(struct DosEntry){ "A file with a long name.txt", 30, 0, 0, 0 });
    snprintf(d.status, sizeof d.status, "%u drawer%s, %u file%s", d.dirs, d.dirs == 1 ? "" : "s", d.files,
             d.files == 1 ? "" : "s");
    CHECK(InitUI(&ui, g) == 0 && wb_drawer_build(&iui, &ui, &d));
    g->InitRastPort(&rp, win);
    ui.ui_RPort = &rp;
    LayoutUI(&ui, WB_DRAWER_W, WB_DRAWER_H);
    DrawUI(&ui);
    ok &= as_kept("drawer", win, DRAWER_CRC, &crc[2], keep);
    FreeUI(&ui);

    /* The HUD on the backdrop: drawn whole, then a new reading drawn in
     * part, which must be what drawing it whole gives. */
    g->InitRastPort(&rp, bm);
    desktop(g, &rp, sans);
    desktop(g, &crp, sans);
    backdrop_copy = copy;
    CHECK(InitUI(&ui, g) == 0);
    ui.ui_Backfill = backfill;
    CHECK(AddGadgetTags(&iui, &ui, 0, UI_COLUMN, { UIA_Pane, UIPANE_BACKFILL }, { UIA_Margin, 16 }) == 1);
    CHECK(AddGadgetTags(&iui, &ui, 1, UI_HUDBADGE, { UIA_Label, (uint64_t)"CPU   0.8%\nRAM  12.5%" },
                        { UIA_Align, UIALIGN_END }) == 2);
    ui.ui_RPort = &rp;
    LayoutUI(&ui, BW, BH);
    DrawUI(&ui);
    ui.ui_NumDamage = 0;
    SetGadgetAttrs(&iui, &ui, 2, { UIA_Label, (uint64_t)"CPU  37.5%\nRAM  12.6%" });
    CHECK(ui.ui_NumDamage == 1 && ui.ui_Damage[0].MaxX == (int32_t)BW - 1 - 16 && ui.ui_Damage[0].MinY == 16);
    ok &= as_kept("hud", bm, HUD_CRC, &crc[3], keep);
    DrawUI(&ui);
    CHECK(crc_of(bm) == crc[3]);
    FreeUI(&ui);

    /* Prefs' window, at the defaults. */
    CHECK(InitUI(&ui, g) == 0 && pw_build(&iui, &ui, &ip, &np, "800 \xc3\x97 600 at 60.0 Hz", "No profiles yet"));
    g->InitRastPort(&rp, pw);
    ui.ui_RPort = &rp;
    LayoutUI(&ui, PW_WIDTH, PW_HEIGHT);
    DrawUI(&ui);
    ok &= as_kept("prefs", pw, PREFS_CRC, &crc[4], keep);
    FreeUI(&ui);

    g->FreeBitMap(pw);
    g->FreeBitMap(win);
    g->FreeBitMap(copy);
    g->FreeBitMap(bm);
    g->CloseTextFont(sans);
    return ok;
}

int main(void)
{
    static const struct FontIFace font = {
        .SystemFont = SystemFont, .OpenFont = OpenFont, .FontInfo = FontInfo, .FontName = FontName,
        .MapCodepoint = MapCodepoint, .GetGlyphMetrics = GetGlyphMetrics, .GlyphOutline = GlyphOutline,
        .ScaleUnits = ScaleUnits, .ShapeRun = ShapeRun, .FitLine = FitLine, .LineSpacing = LineSpacing,
    };
    uint32_t crc[5];

    gfx_host_ifont = &font;
    if (!run(&scalar, true, crc) || failures)
        return 1;
#ifdef GFX_TEST_NEON
    {
        uint32_t ncrc[5];

        if (!run(&neon, false, ncrc) || failures || memcmp(crc, ncrc, sizeof crc) != 0) {
            printf("wb-test: FAILED: NEON doesn't draw the desktop as the scalar code does\n");
            return 1;
        }
    }
#endif
    printf("wb-test: ok: the backdrop and two volumes' icons (%08x), one selected (%08x), a volume's window "
           "(%08x), the HUD on the backdrop, drawn again in part as whole (%08x), Prefs' window (%08x)%s\n",
           crc[0], crc[1], crc[2], crc[3], crc[4],
#ifdef GFX_TEST_NEON
           ", scalar and NEON alike"
#else
           ""
#endif
    );
    return 0;
}
