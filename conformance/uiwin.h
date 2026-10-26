/*
 * conformance/uiwin.h -- a window of every gadget, as conformance/window.c
 * clicks through it on QEMU and lib/ui/test.c does on the host (V8 step 6)
 *
 * uw_build makes the tree: a split pane, its dark side the controls (a
 * label, the three toggles, a divider, a slider, the HUD badge), its
 * light side the rest (a label, the segmented selector, a list, a text
 * field, two buttons). uw_script is what's done to it, a step at a time,
 * and what the last change a step reports must be. The host makes the
 * steps' events itself; QEMU's are boot-check's, sent as the step says.
 * The inside is read back by CRC when it's first drawn (uw_crc_ready)
 * and after the script (uw_crc_done), against the host's.
 */
#ifndef CONFORMANCE_UIWIN_H
#define CONFORMANCE_UIWIN_H

#include <stdint.h>

#include <planar/ui.h>

constexpr int32_t UW_LEFT = 120, UW_TOP = 70;  /* the window's chrome's top left */
constexpr uint32_t UW_WIDTH = 600, UW_HEIGHT = 380;    /* its inside */
constexpr uint32_t UW_FIELD = 32;               /* the text field's buffer */

/* The gadgets' numbers, as uw_build adds them. */
enum : uint32_t {
    UW_SPLIT = 1, UW_LEFTCOL, UW_CONTROLS, UW_REPEAT, UW_MIXER, UW_LOCKED, UW_DIVIDER, UW_SPEED, UW_BADGE,
    UW_RIGHTCOL, UW_MODES, UW_MODE, UW_FILES, UW_NAME, UW_BUTTONS, UW_CANCEL, UW_SAVE,
};

static const char *const uw_modes[] = { "Active", "Normal", "Quiet" };
static const char *const uw_files[] = { "C", "Devs", "Fonts", "Prefs", "S", "System", "Tools", "Utilities" };

static inline bool uw_build(const struct UIIFace *iui, struct UI *ui, char *field)
{
    uint32_t n = 0;

    field[0] = 0;
    n += AddGadgetTags(iui, ui, 0, UI_SPLITPANE, { UIA_Value, 240 }) == UW_SPLIT;
    n += AddGadgetTags(iui, ui, UW_SPLIT, UI_COLUMN, { UIA_Margin, 14 }, { UIA_Spacing, 10 }) == UW_LEFTCOL;
    n += AddGadgetTags(iui, ui, UW_LEFTCOL, UI_LABEL, { UIA_Label, (uint64_t)"Controls" }) == UW_CONTROLS;
    n += AddGadgetTags(iui, ui, UW_LEFTCOL, UI_TOGGLE, { UIA_Label, (uint64_t)"Key repeat" }) == UW_REPEAT;
    n += AddGadgetTags(iui, ui, UW_LEFTCOL, UI_TOGGLE, { UIA_Label, (uint64_t)"Mixer" },
                       { UIA_Variant, TOGGLE_AMBER }, { UIA_Glyph, (uint64_t)"L/R" }) == UW_MIXER;
    n += AddGadgetTags(iui, ui, UW_LEFTCOL, UI_TOGGLE, { UIA_Label, (uint64_t)"Locked" }, { UIA_Value, 1 },
                       { UIA_Disabled, 1 }) == UW_LOCKED;
    n += AddGadgetTags(iui, ui, UW_LEFTCOL, UI_DIVIDER, { UIA_Height, 6 }) == UW_DIVIDER;
    n += AddGadgetTags(iui, ui, UW_LEFTCOL, UI_SLIDER, { UIA_Label, (uint64_t)"Speed" }, { UIA_Min, 0 },
                       { UIA_Max, 20 }, { UIA_Value, 5 }) == UW_SPEED;
    n += AddGadgetTags(iui, ui, UW_LEFTCOL, UI_HUDBADGE, { UIA_Label, (uint64_t)"CPU  0.8%\nRAM 0.00%" },
                       { UIA_Align, UIALIGN_END }) == UW_BADGE;
    n += AddGadgetTags(iui, ui, UW_SPLIT, UI_COLUMN, { UIA_Margin, 14 }, { UIA_Spacing, 10 }) == UW_RIGHTCOL;
    n += AddGadgetTags(iui, ui, UW_RIGHTCOL, UI_LABEL, { UIA_Label, (uint64_t)"Modes" }) == UW_MODES;
    n += AddGadgetTags(iui, ui, UW_RIGHTCOL, UI_SEGMENTED, { UIA_Items, (uint64_t)uw_modes }, { UIA_Count, 3 },
                       { UIA_Value, 1 }) == UW_MODE;
    n += AddGadgetTags(iui, ui, UW_RIGHTCOL, UI_LIST, { UIA_Items, (uint64_t)uw_files }, { UIA_Count, 8 },
                       { UIA_Weight, 1 }) == UW_FILES;
    n += AddGadgetTags(iui, ui, UW_RIGHTCOL, UI_TEXTFIELD, { UIA_Buffer, (uint64_t)field },
                       { UIA_Size, UW_FIELD }) == UW_NAME;
    n += AddGadgetTags(iui, ui, UW_RIGHTCOL, UI_ROW, { UIA_Align, UIALIGN_END }) == UW_BUTTONS;
    n += AddGadgetTags(iui, ui, UW_BUTTONS, UI_BUTTON, { UIA_Label, (uint64_t)"Cancel" }) == UW_CANCEL;
    n += AddGadgetTags(iui, ui, UW_BUTTONS, UI_BUTTON, { UIA_Label, (uint64_t)"Save" }) == UW_SAVE;
    return n == UW_SAVE;
}

/* A step: where in a gadget (px from its rectangle's top left; negative,
 * from its right or bottom), and what the last change it reports must be. */
enum : uint8_t { UW_CLICK, UW_DRAG, UW_TEXT, UW_KEY };

struct uw_step {
    uint8_t action;
    uint32_t gadget;
    int32_t dx, dy;
    int32_t drag;                       /* UW_DRAG: across, px, the button held */
    const char *text;                   /* UW_TEXT's; UW_KEY's: boot-check's key, "ret", "tab", "spc", "right" */
    uint32_t want;                      /* the gadget changed; 0 none */
    int32_t value;
    uint32_t flags;
};

/* A list's row's middle, from its rectangle's top: its focus room, edge, rows. */
#define UW_ROW(i) (2 + 1 + (i) * 22 + 11)

static const struct uw_step uw_script[] = {
    { UW_CLICK, UW_REPEAT, -20, 13, 0, nullptr, UW_REPEAT, 1, 0 },
    { UW_CLICK, UW_MIXER, -20, 13, 0, nullptr, UW_MIXER, 1, 0 },
    { UW_CLICK, UW_LOCKED, -20, 13, 0, nullptr, 0, 0, 0 },              /* disabled: deaf */
    { UW_CLICK, UW_SPEED, -10, 11, 0, nullptr, UW_SPEED, 20, 0 },       /* the track's right end */
    { UW_KEY, 0, 0, 0, 0, "left", UW_SPEED, 19, 0 },
    { UW_CLICK, UW_MODE, -20, 15, 0, nullptr, UW_MODE, 2, 0 },
    { UW_KEY, 0, 0, 0, 0, "left", UW_MODE, 1, 0 },
    { UW_CLICK, UW_FILES, 40, UW_ROW(3), 0, nullptr, UW_FILES, 3, 0 },
    { UW_CLICK, UW_FILES, 40, UW_ROW(3), 0, nullptr, UW_FILES, 3, UICF_DOUBLE },
    { UW_KEY, 0, 0, 0, 0, "down", UW_FILES, 4, 0 },
    { UW_CLICK, UW_NAME, 20, 14, 0, nullptr, 0, 0, 0 },
    { UW_TEXT, 0, 0, 0, 0, "Planar Prefs", 0, 0, 0 },
    { UW_KEY, 0, 0, 0, 0, "ret", UW_NAME, 12, UICF_ENTER },
    { UW_KEY, 0, 0, 0, 0, "tab", 0, 0, 0 },                             /* to Cancel */
    { UW_KEY, 0, 0, 0, 0, "spc", UW_CANCEL, 1, 0 },
    { UW_DRAG, UW_SPEED, -10, 11, -60, nullptr, UW_SPEED, 12, 0 },      /* from the knob, near the right */
    { UW_CLICK, UW_SAVE, 30, 14, 0, nullptr, UW_SAVE, 1, 0 },
};

/* A step's point, in the inside's coordinates. */
static inline void uw_point(const struct UI *ui, const struct uw_step *s, int32_t *x, int32_t *y)
{
    struct GfxRect r = ui->ui_Gadget[s->gadget].g_Rect;

    *x = s->dx >= 0 ? r.MinX + s->dx : r.MaxX + 1 + s->dx;
    *y = s->dy >= 0 ? r.MinY + s->dy : r.MaxY + 1 + s->dy;
}

constexpr uint32_t uw_crc_ready = 0x57b558ce;
constexpr uint32_t uw_crc_done = 0x541b4b74;

#endif /* CONFORMANCE_UIWIN_H */
