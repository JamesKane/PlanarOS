/*
 * planar/ui.h -- ui.library: gadgets drawn in the caller's window (V8
 * step 6; docs §13.3)
 *
 * A retained toolkit in the caller's domain (§13.1): the gadgets are a
 * tree in the caller's struct UI, drawn with the caller's gfx.library
 * into its window's buffer, and driven by its IDCMP events. intuition
 * knows nothing of them: a gadget is the client's pixels. The library
 * keeps nothing of its own.
 *
 * The caller:
 *     InitUI(ui, IGfx);                       the fonts opened
 *     root = AddGadgetTags(IUI, ui, 0, UI_COLUMN, { UIA_Margin, 12 });
 *     ...  AddGadgetTags(IUI, ui, root, UI_TOGGLE, { UIA_Label, (uint64_t)"Repeat" });
 *     ui->ui_RPort = &w->w_RPort;
 *     LayoutUI(ui, w->w_Width, w->w_Height);  at the start and IDCMP_NEWSIZE
 *     DrawUI(ui);                             the whole inside
 *     PresentWindow(w, ui->ui_Damage, ui->ui_NumDamage); ui->ui_NumDamage = 0;
 * and for each event:
 *     if (HandleUIEvent(ui, &ev, &change)) ... change.uc_Gadget, uc_Value
 *     then presents the damage, as above.
 *
 * Gadgets are numbered from 1 in the order added; the first is the root,
 * laid out over the whole inside. A gadget's strings, a list's items and
 * a text field's buffer are the caller's, and must outlive it.
 */
#ifndef PLANAR_UI_H
#define PLANAR_UI_H

#include <stdint.h>

#include <planar/exec.h>
#include <planar/gfx.h>
#include <planar/intuition.h>
#include <planar/utility.h>

constexpr unsigned UI_GADGETS = 48;         /* a UI's gadgets, at most */
constexpr unsigned UI_DAMAGE = INTUI_RECTS; /* rectangles of damage, at most: then one around them all */

/* What a gadget is: §13.3's six, decision 8's four, and two groups. */
enum : uint8_t {
    UI_ROW = 1,         /* children left to right */
    UI_COLUMN = 2,      /* children top to bottom */
    UI_SPLITPANE = 3,   /* two children, a dark pane and a light one; uc_Value the left's width */
    UI_DIVIDER = 4,     /* a 2 px groove across */
    UI_TOGGLE = 5,      /* a pill switch: uc_Value 0 or 1 */
    UI_SLIDER = 6,      /* uc_Value from UIA_Min to UIA_Max */
    UI_SEGMENTED = 7,   /* UIA_Items' options, one chosen: uc_Value its index */
    UI_HUDBADGE = 8,    /* a card of System Mono lines, right-aligned: UIA_Label, split at '\n' */
    UI_BUTTON = 9,      /* uc_Value 1 when clicked */
    UI_LABEL = 10,      /* text */
    UI_LIST = 11,       /* UIA_Items, one selected: uc_Value its index, -1 none */
    UI_TEXTFIELD = 12,  /* a line edited in UIA_Buffer */
};

/* UIA_Variant: a toggle's look; a label's alignment. */
enum : uint8_t {
    TOGGLE_COBALT = 0,  /* blue when on */
    TOGGLE_AMBER = 1,   /* amber when on, with UIA_Glyph on its track */
    ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2,
};

/* UIA_Align: a group's children across it (a row's heights, a column's widths). */
enum : uint8_t {
    UIALIGN_INHERIT = 0,    /* a child's: as its group says */
    UIALIGN_FILL = 1,       /* all of it: a column's default */
    UIALIGN_START = 2,
    UIALIGN_CENTER = 3,     /* a row's default */
    UIALIGN_END = 4,
};

/* UIA_Pane: what a group is drawn on. A split pane's children are dark
 * and light; the root, if it says nothing, light. */
enum : uint8_t { UIPANE_NONE = 0, UIPANE_DARK = 1, UIPANE_LIGHT = 2 };

constexpr Tag UIA_Dummy = TAG_USER + 0x600;
constexpr Tag UIA_Label = UIA_Dummy + 1;    /* const char *: a label, a button's or a toggle's, a badge's lines */
constexpr Tag UIA_Value = UIA_Dummy + 2;    /* int32_t: 0 */
constexpr Tag UIA_Min = UIA_Dummy + 3;      /* int32_t: a slider's, 0 */
constexpr Tag UIA_Max = UIA_Dummy + 4;      /* int32_t: a slider's, 100 */
constexpr Tag UIA_Items = UIA_Dummy + 5;    /* const char *const *: options, a list's rows */
constexpr Tag UIA_Count = UIA_Dummy + 6;    /* uint32_t: UIA_Items' */
constexpr Tag UIA_Buffer = UIA_Dummy + 7;   /* char *: a text field's line, ended by 0 */
constexpr Tag UIA_Size = UIA_Dummy + 8;     /* uint32_t: UIA_Buffer's bytes, its end's among them */
constexpr Tag UIA_Weight = UIA_Dummy + 9;   /* uint32_t: a share of the group's room left over: 0 */
constexpr Tag UIA_Spacing = UIA_Dummy + 10; /* uint32_t: a group's between children: 8 */
constexpr Tag UIA_Margin = UIA_Dummy + 11;  /* uint32_t: a group's around them: 0 */
constexpr Tag UIA_Width = UIA_Dummy + 12;   /* uint32_t: the least width, over the gadget's own */
constexpr Tag UIA_Height = UIA_Dummy + 13;  /* uint32_t: the least height */
constexpr Tag UIA_Variant = UIA_Dummy + 14; /* TOGGLE_*, ALIGN_* */
constexpr Tag UIA_Disabled = UIA_Dummy + 15;    /* bool: drawn muted, and deaf */
constexpr Tag UIA_Align = UIA_Dummy + 16;   /* UIALIGN_* */
constexpr Tag UIA_Pane = UIA_Dummy + 17;    /* UIPANE_* */
constexpr Tag UIA_Glyph = UIA_Dummy + 18;   /* const char *: an amber toggle's, on its track */

struct UIGadget {
    uint8_t g_Kind;                     /* UI_*; 0, unused */
    uint8_t g_Parent, g_Child, g_Next;  /* the tree: gadgets' numbers, 0 none */
    uint8_t g_Variant, g_Align, g_Pane;
    uint8_t g_Disabled;
    struct GfxRect g_Rect;              /* laid out: the inside's coordinates, inclusive */
    int32_t g_Value, g_Min, g_Max;
    int32_t g_Top;                      /* a list's first row shown; a text field's scroll, px */
    int32_t g_Cursor;                   /* a text field's, a byte in g_Buffer */
    uint32_t g_Weight, g_Spacing, g_Margin;
    uint32_t g_Width, g_Height;         /* UIA_Width, UIA_Height */
    int32_t g_NatW, g_NatH;             /* its own size, with its children's, as LayoutUI measured it */
    const char *g_Label, *g_Glyph;
    const char *const *g_Items;
    uint32_t g_Count, g_Size;
    char *g_Buffer;
};

/* HandleUIEvent's: which gadget changed, and how. */
struct UIChange {
    uint32_t uc_Gadget;
    int32_t uc_Value;
    uint32_t uc_Flags;                  /* UICF_* */
};

constexpr uint32_t UICF_DOUBLE = 1u << 0;   /* a list's row double-clicked, or Return on it */
constexpr uint32_t UICF_ENTER = 1u << 1;    /* Return in a text field */
constexpr uint32_t UICF_DRAG = 1u << 2;     /* a slider or the split dragged, the button still down */

constexpr uint64_t UI_DOUBLE_NS = 500000000;    /* two clicks on a row this close are a double click */

struct UI {
    const struct GfxIFace *ui_Gfx;
    struct RastPort *ui_RPort;          /* where it draws: the caller's, set before DrawUI */
    struct TextFont *ui_Sans, *ui_Bold, *ui_Mono;   /* InitUI's: System Sans and Mono, 13 px */
    uint32_t ui_Width, ui_Height;       /* LayoutUI's */
    uint32_t ui_Count;                  /* gadgets added */
    uint32_t ui_Focus;                  /* the gadget keys go to; 0 none */
    uint32_t ui_Pressed;                /* the gadget the button went down on; 0 none */
    uint32_t ui_Part;                   /* what of it: a segment, a row; the split's groove */
    int32_t ui_GrabX;                   /* a drag's pointer from the thumb's middle */
    bool ui_Inside;                     /* a pressed button with the pointer on it */
    uint32_t ui_ClickGadget;            /* the last click on a list's row, for a double */
    int32_t ui_ClickRow;
    uint64_t ui_ClickTime;
    uint32_t ui_NumDamage;              /* the caller presents and zeroes */
    struct GfxRect ui_Damage[UI_DAMAGE];
    struct GfxPoint ui_Points[40];      /* a path's, while drawing */
    uint8_t ui_Verbs[24];
    struct UIGadget ui_Gadget[1 + UI_GADGETS];  /* [0] unused */
};

/*
 * ui.library's "main" interface, version 1. A program opens it:
 *
 *     struct Library *lib = IExec->OpenLibrary("ui.library", 8);
 *     struct UIIFace *IUI =
 *         lib ? (struct UIIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
 */
struct UIIFace {
    struct Interface Data;

    int64_t (*InitUI)(struct UI *ui, const struct GfxIFace *gfx);
    void (*FreeUI)(struct UI *ui);
    uint32_t (*AddGadget)(struct UI *ui, uint32_t parent, uint32_t kind, const struct TagItem *tags);
    void (*SetGadgetAttrsA)(struct UI *ui, uint32_t gadget, const struct TagItem *tags);
    void (*LayoutUI)(struct UI *ui, uint32_t width, uint32_t height);
    void (*DrawUI)(struct UI *ui);
    uint32_t (*HandleUIEvent)(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *change);
};

#define AddGadgetTags(iface, ui, parent, kind, ...) \
    ((iface)->AddGadget((ui), (parent), (kind), (const struct TagItem[]){ __VA_ARGS__, { TAG_END, 0 } }))
#define SetGadgetAttrs(iface, ui, gadget, ...) \
    ((iface)->SetGadgetAttrsA((ui), (gadget), (const struct TagItem[]){ __VA_ARGS__, { TAG_END, 0 } }))

/* Zeroes the UI and opens its fonts with 'gfx': 0, or ERR_NOTFOUND if
 * System Sans or Mono wouldn't open. */
int64_t InitUI(struct UI *ui, const struct GfxIFace *gfx);
/* Closes its fonts. */
void FreeUI(struct UI *ui);
/* A gadget of this kind, the last of parent's children (0: the root,
 * the first gadget, which nothing else may be): its number, or 0 if
 * the UI's full, the kind's unknown, or the parent isn't a group (a
 * split pane takes two). */
uint32_t AddGadget(struct UI *ui, uint32_t parent, uint32_t kind, const struct TagItem *tags);
/* Its attributes changed, and it's drawn again if it's been laid out:
 * a size it measures by isn't relaid out until LayoutUI. */
void SetGadgetAttrsA(struct UI *ui, uint32_t gadget, const struct TagItem *tags);
/* Every gadget measured and placed in an inside of this size. */
void LayoutUI(struct UI *ui, uint32_t width, uint32_t height);
/* Every gadget drawn in ui_RPort, the whole inside its damage. */
void DrawUI(struct UI *ui);
/* An IDCMP event: IDCMP_MOUSEBUTTONS, IDCMP_MOUSEMOVE, IDCMP_VANILLAKEY
 * and IDCMP_RAWKEY are read, others ignored. Whatever it changes is drawn
 * again and its rectangles added to ui_Damage. The gadget whose value
 * changed, its value and how in *change; or 0. */
uint32_t HandleUIEvent(struct UI *ui, const struct IntuiEvent *ev, struct UIChange *change);

#endif /* PLANAR_UI_H */
