/*
 * servers/prefs/prefswin.h -- Prefs' window, for Prefs and for the tests
 * that work it (conformance/window.c on QEMU, servers/workbench/test.c
 * on the host) (V8 step 7)
 *
 * One window, a split pane (§13.5). Its dark side is input's settings,
 * a slider each, and then what display and power have that a user will
 * set once they can, said read-only (decision 5). Its light side is
 * intuition's: the HUD, the pointer's size, and the backdrop's colours,
 * one of three; then Cancel, Use and Save. Each change goes to its
 * server at once; Save writes the files too (planar/prefs.h); Cancel
 * puts back what was in force when Prefs opened; Use keeps what's in
 * force and saves nothing. The close gadget is Cancel.
 */
#ifndef SERVERS_PREFS_PREFSWIN_H
#define SERVERS_PREFS_PREFSWIN_H

#include <stdint.h>

#include <planar/devices/input.h>
#include <planar/intuition.h>
#include <planar/ui.h>

constexpr int32_t PW_LEFT = 96, PW_TOP = 90;    /* the window's chrome's top left */
constexpr uint32_t PW_WIDTH = 600, PW_HEIGHT = 360;  /* its inside */

/* The gadgets' numbers, as pw_build adds them. */
enum : uint32_t {
    PW_SPLIT = 1, PW_INPUTCOL, PW_INPUT, PW_DELAY, PW_RATE, PW_SPEED, PW_ACCEL, PW_DIVIDER, PW_DISPLAY,
    PW_DISPLAY_INFO, PW_POWER, PW_POWER_INFO, PW_SCREENCOL, PW_SCREEN, PW_HUD, PW_POINTER, PW_SIZE, PW_BACKDROP,
    PW_THEME, PW_SPACE, PW_BUTTONS, PW_CANCEL, PW_USE, PW_SAVE,
};

static const char *const pw_sizes[] = { "Auto", "Small", "Large" };
static const char *const pw_themes[] = { "Twilight", "Ocean", "Graphite" };

/* The backdrops' colours: §13.2's first. */
static const uint32_t pw_theme_top[] = { INTUI_BACKDROP_TOP, 0x0c2a3a, 0x1c2026 };
static const uint32_t pw_theme_bottom[] = { INTUI_BACKDROP_BOTTOM, 0x15404e, 0x2c3038 };

/* The theme of a backdrop's colours; the first if it's none of them. */
static inline uint32_t pw_theme_of(const struct IntuiPrefs *p)
{
    for (uint32_t i = 0; i < 3; i++)
        if (p->ip_BackdropTop == pw_theme_top[i] && p->ip_BackdropBottom == pw_theme_bottom[i])
            return i;
    return 0;
}

static inline bool pw_build(const struct UIIFace *iui, struct UI *ui, const struct InputPrefs *ip,
                            const struct IntuiPrefs *np, const char *display, const char *power)
{
    uint32_t n = 0;

    n += AddGadgetTags(iui, ui, 0, UI_SPLITPANE, { UIA_Value, 300 }) == PW_SPLIT;
    n += AddGadgetTags(iui, ui, PW_SPLIT, UI_COLUMN, { UIA_Margin, 14 }, { UIA_Spacing, 10 }) == PW_INPUTCOL;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_LABEL, { UIA_Label, (uint64_t)"Input" }) == PW_INPUT;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_SLIDER, { UIA_Label, (uint64_t)"Repeat delay" },
                       { UIA_Min, INPUT_DELAY_MIN }, { UIA_Max, INPUT_DELAY_MAX },
                       { UIA_Value, ip->ip_RepeatDelay }) == PW_DELAY;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_SLIDER, { UIA_Label, (uint64_t)"Repeat rate" },
                       { UIA_Min, INPUT_RATE_MIN }, { UIA_Max, INPUT_RATE_MAX },
                       { UIA_Value, ip->ip_RepeatRate }) == PW_RATE;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_SLIDER, { UIA_Label, (uint64_t)"Pointer speed" },
                       { UIA_Min, INPUT_SPEED_MIN }, { UIA_Max, INPUT_SPEED_MAX },
                       { UIA_Value, ip->ip_PointerSpeed }) == PW_SPEED;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_SLIDER, { UIA_Label, (uint64_t)"Acceleration" },
                       { UIA_Min, INPUT_ACCEL_MIN }, { UIA_Max, INPUT_ACCEL_MAX },
                       { UIA_Value, ip->ip_PointerAccel }) == PW_ACCEL;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_DIVIDER, { UIA_Height, 6 }) == PW_DIVIDER;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_LABEL, { UIA_Label, (uint64_t)"Display" }) == PW_DISPLAY;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_LABEL, { UIA_Label, (uint64_t)display }) == PW_DISPLAY_INFO;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_LABEL, { UIA_Label, (uint64_t)"Power" }) == PW_POWER;
    n += AddGadgetTags(iui, ui, PW_INPUTCOL, UI_LABEL, { UIA_Label, (uint64_t)power }) == PW_POWER_INFO;
    n += AddGadgetTags(iui, ui, PW_SPLIT, UI_COLUMN, { UIA_Margin, 14 }, { UIA_Spacing, 10 }) == PW_SCREENCOL;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_LABEL, { UIA_Label, (uint64_t)"Screen" }) == PW_SCREEN;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_TOGGLE, { UIA_Label, (uint64_t)"CPU and memory badge" },
                       { UIA_Value, np->ip_Hud }) == PW_HUD;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_LABEL, { UIA_Label, (uint64_t)"Pointer" }) == PW_POINTER;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_SEGMENTED, { UIA_Items, (uint64_t)pw_sizes }, { UIA_Count, 3 },
                       { UIA_Value, np->ip_PointerSize }) == PW_SIZE;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_LABEL, { UIA_Label, (uint64_t)"Backdrop" }) == PW_BACKDROP;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_SEGMENTED, { UIA_Items, (uint64_t)pw_themes }, { UIA_Count, 3 },
                       { UIA_Value, pw_theme_of(np) }) == PW_THEME;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_LABEL, { UIA_Label, (uint64_t)"" }, { UIA_Weight, 1 }) == PW_SPACE;
    n += AddGadgetTags(iui, ui, PW_SCREENCOL, UI_ROW, { UIA_Align, UIALIGN_END }) == PW_BUTTONS;
    n += AddGadgetTags(iui, ui, PW_BUTTONS, UI_BUTTON, { UIA_Label, (uint64_t)"Cancel" }) == PW_CANCEL;
    n += AddGadgetTags(iui, ui, PW_BUTTONS, UI_BUTTON, { UIA_Label, (uint64_t)"Use" }) == PW_USE;
    n += AddGadgetTags(iui, ui, PW_BUTTONS, UI_BUTTON, { UIA_Label, (uint64_t)"Save" }) == PW_SAVE;
    return n == PW_SAVE;
}

/* A point of a gadget, as uiwin.h's are: px from its rectangle's top
 * left, or (negative) from its right or bottom. */
static inline void pw_point(const struct UI *ui, uint32_t gadget, int32_t dx, int32_t dy, int32_t *x, int32_t *y)
{
    struct GfxRect r = ui->ui_Gadget[gadget].g_Rect;

    *x = dx >= 0 ? r.MinX + dx : r.MaxX + 1 + dx;
    *y = dy >= 0 ? r.MinY + dy : r.MaxY + 1 + dy;
}

#endif /* SERVERS_PREFS_PREFSWIN_H */
