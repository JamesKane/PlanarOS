/*
 * planar/intuition.h -- windows on the screen (v7-plan steps 3 and 4)
 *
 * intuition (servers/intuition) composes one screen, the Workbench's,
 * on display's plane 1, over the console on plane 0, with the pointer on
 * plane 2. It publishes "intuition". A window's pixels are the client's
 * own (docs §13.1): bitmaps it draws in with gfx.library and shares with
 * intuition read-only. intuition composes from them on the CPU, into
 * two screen buffers of its own shown in turn, and never writes a
 * client's memory. Around them it draws each window's chrome (§13.3),
 * its own pixels: the title bar, the gadgets and the frame.
 *
 * A client presents a frame of a window: which of its buffers to show,
 * what changed since the last frame (up to INTUI_RECTS rectangles; none
 * is the whole window), and the frame's number, above the last. Each
 * window has a line of its connection's page, shared read-only with the
 * client, its sync words (§16.3 pillar 4):
 *   is_Disp, the last frame whose pixels are on the screen, latched at
 *            the vblank of is_DispTime;
 *   is_Safe, the last frame intuition has composed: from then on it
 *            reads no buffer that only frames before it showed. A client
 *            with two buffers draws in the other once is_Safe has
 *            reached the frame it last presented;
 *   is_LeftEdge, is_TopEdge, where the window is, dragged or not.
 * Each of is_Disp and is_Safe is woken with WakeAddr when it moves.
 *
 * intuition holds input's ring (planar/devices/input.h): the pointer is
 * its own, and a button pressed over a window makes it the active one,
 * at the front. Keys go to the active window. What a window's client
 * asked for of that (its IDCMP classes) comes as IntuiEvents on a ring
 * in the same page, written by intuition at ip_Head; the client says how
 * far it has read in a word of its own, ia_Tail, on a page it shares
 * with intuition read-only (INTUI_EVENTS). intuition writes no event the
 * client hasn't room for, nor a mouse move past INTUI_EVENTS_MOVES unread:
 * it keeps them, and past what it keeps it drops the oldest mouse moves,
 * then whatever comes, counting them in ip_Lost. A client that reads the
 * ring down from full, or from INTUI_EVENTS_MOVES, says so with
 * SignalConn on its connection, so intuition writes what it kept.
 *
 * The client's side is intuition.library: IIntuition's OpenIntuition,
 * OpenWindowTagList, PresentWindow, WaitSafe, CloseWindow,
 * GetIntuiEvent and WaitIntuiEvent, over the messages below. Its state
 * is the caller's: an Intuition for the connection, a Window for each
 * window.
 */
#ifndef PLANAR_INTUITION_H
#define PLANAR_INTUITION_H

#include <stdint.h>

#include <planar/exec.h>
#include <planar/gfx.h>
#include <planar/msg.h>
#include <planar/utility.h>

constexpr unsigned INTUI_RECTS = 16;        /* a present's damage, at most */
constexpr unsigned INTUI_BUFFERS = 2;       /* a window's buffers, at most */
constexpr unsigned INTUI_TITLE = 32;        /* a title's bytes, its end included */
constexpr unsigned INTUI_WINDOWS = 16;      /* a connection's windows, at most */

/* The chrome around a window that isn't WFLG_BORDERLESS (§13.3): the
 * title bar above its inside, the frame on its other three sides. */
constexpr uint32_t INTUI_TITLEBAR = 26;
constexpr uint32_t INTUI_BORDER = 4;

/* IntuiMsg.im_Op */
enum : uint32_t {
    INTUI_INFO = 1,     /* im_Task in; im_Server, the screen's im_Width and im_Height, state out */
    INTUI_OPEN = 2,     /* im_Task, the window and its buffers in; im_Window, im_Sync out */
    INTUI_CLOSE = 3,    /* im_Window in */
    INTUI_PRESENT = 4,  /* im_Window, im_Index, im_Frame, im_Count rectangles in im_Rect */
    INTUI_EVENTS = 5,   /* im_Task, im_Ack in; im_Page out */
};

/* A buffer of a window: memory of the client's, shared with intuition
 * (its INTUI_INFO's im_Server) read-only before INTUI_OPEN. Its top
 * left im_Width by im_Height pixels are the window's inside. */
struct IntuiBuffer {
    uint64_t ib_Address;
    uint32_t ib_BytesPerRow;
    uint32_t ib_Format;                 /* PIXFMT_*: shown opaque either way, in V7 */
    uint32_t ib_Width, ib_Height;
};

struct IntuiMsg {
    struct Message im_Msg;
    uint32_t im_Op;
    uint32_t im_Window;                 /* intuition's number for it */
    Handle im_Task;                     /* the client's own task (FindTask(nullptr)) */
    uint32_t im_Flags;                  /* INTUI_OPEN: WFLG_* */
    int32_t im_Left, im_Top;            /* INTUI_OPEN: where on the screen, its chrome's top left */
    uint32_t im_Width, im_Height;       /* INTUI_OPEN: the inside's; INTUI_INFO: the screen's */
    uint32_t im_Buffers;                /* INTUI_OPEN: in im_Buffer, 1 or 2 */
    uint32_t im_Index;                  /* INTUI_PRESENT: which buffer */
    struct IntuiBuffer im_Buffer[INTUI_BUFFERS];
    char im_Title[INTUI_TITLE];
    uint64_t im_Sync;                   /* INTUI_OPEN: its IntuiSync, mapped read-only */
    uint64_t im_Server;                 /* INTUI_INFO: intuition's task, to share with */
    uint64_t im_Frame;                  /* INTUI_PRESENT: above the last presented */
    uint32_t im_Count;                  /* INTUI_PRESENT: in im_Rect; 0, the whole window */
    uint32_t im_IDCMP;                  /* INTUI_OPEN: the IDCMP_* classes wanted */
    struct GfxRect im_Rect[INTUI_RECTS];    /* the window's own coordinates, inclusive */
    uint64_t im_Ack;                    /* INTUI_EVENTS: the client's IntuiAck, shared read-only */
    uint64_t im_Page;                   /* INTUI_EVENTS: the connection's IntuiPage, read-only */
    /* INTUI_INFO, for tests and costs: since intuition started. */
    uint64_t im_Frames;                 /* composed */
    uint64_t im_Pixels;                 /* painted composing them */
    uint64_t im_Nanos;                  /* spent composing them */
    int32_t im_PointerX, im_PointerY;   /* now, on the screen */
    uint32_t im_Active;                 /* the active window's number, of any client's; 0 none */
    uint32_t im_Windows;                /* on the screen, every client's */
    uint32_t im_Input;                  /* 1 if intuition holds input's ring */
    uint32_t im_InputOpens;             /* input's rings it has opened: one more each time input's back */
    uint64_t im_Moves;                  /* frames that moved the pointer */
    uint64_t im_MoveNanos, im_MoveMax;  /* their moves' age when latched: input's ie_Time to the latch */
    uint64_t im_ReadMax;                /* a move's age at most when intuition read it from the ring */
    uint64_t im_MoveGaps[4];            /* moves after the last by (ie_Time) <1 ms, <20 ms, <100 ms, <1 s */
};

/* A window's sync words: a cache line of its connection's page. */
struct IntuiSync {
    volatile uint64_t is_Disp;
    volatile uint64_t is_DispTime;
    volatile uint64_t is_Safe;
    volatile int32_t is_LeftEdge, is_TopEdge;   /* its chrome's top left, as dragged */
    uint64_t is_Pad[4];
};

static_assert(sizeof(struct IntuiSync) == 64, "a window's sync words are a cache line");

/* --- IDCMP: what a window's client hears of it. --- */

/* IntuiEvent.iev_Class, and WA_IDCMP's bits: the RKM's. */
constexpr uint32_t IDCMP_MOUSEBUTTONS = 0x00000008;    /* iev_Code SELECTDOWN ...; at iev_MouseX, Y */
constexpr uint32_t IDCMP_MOUSEMOVE = 0x00000010;       /* the active window's: the pointer moved */
constexpr uint32_t IDCMP_CLOSEWINDOW = 0x00000200;     /* the close gadget was clicked */
constexpr uint32_t IDCMP_RAWKEY = 0x00000400;          /* iev_Code a key's usage, | RAWKEY_UP */
constexpr uint32_t IDCMP_ACTIVEWINDOW = 0x00040000;
constexpr uint32_t IDCMP_INACTIVEWINDOW = 0x00080000;
constexpr uint32_t IDCMP_VANILLAKEY = 0x00200000;      /* iev_Code the character a key typed */

/* IDCMP_MOUSEBUTTONS' codes: the RKM's. */
enum : uint16_t {
    SELECTDOWN = 0x68, MENUDOWN = 0x69, MIDDLEDOWN = 0x6a,
    SELECTUP = 0xe8, MENUUP = 0xe9, MIDDLEUP = 0xea,
};

/* IDCMP_RAWKEY: a key let go. Planar's, as usages run past 0x80. */
constexpr uint16_t RAWKEY_UP = 0x8000;

/* IntuiEvent.iev_Qualifier: input's IEQ_* (planar/devices/input.h),
 * and these, at the RKM's IEQUALIFIER_* bits. */
constexpr uint16_t IEQ_REPEAT = 1u << 9;    /* input's repeat of a held key */
constexpr uint16_t IEQ_MIDBUTTON = 1u << 12, IEQ_RBUTTON = 1u << 13, IEQ_LEFTBUTTON = 1u << 14;

struct IntuiEvent {
    uint64_t iev_Time;                  /* CurrentTime's, as input had it */
    uint32_t iev_Class;                 /* one IDCMP_* */
    uint32_t iev_Window;                /* intuition's number for it: its Window's w_Id */
    uint16_t iev_Code;
    uint16_t iev_Qualifier;
    uint32_t iev_Char;                  /* a key going down: what it types, or 0 */
    int32_t iev_MouseX, iev_MouseY;     /* the pointer, in the window's inside's coordinates */
};

static_assert(sizeof(struct IntuiEvent) == 32, "an event is 32 bytes");

constexpr unsigned INTUI_EVENTS_RING = 94;     /* events: what's left of the page */
constexpr unsigned INTUI_EVENTS_MOVES = 47;    /* of them mouse moves, at most */

/* A connection's page, intuition's, shared read-only with its client. */
struct IntuiPage {
    struct IntuiSync ip_Sync[INTUI_WINDOWS];
    volatile uint64_t ip_Head;          /* events written, ever: the next at ip_Head % the ring */
    volatile uint64_t ip_Lost;          /* events dropped, ever: the client was too far behind */
    uint64_t ip_Pad[6];
    struct IntuiEvent ip_Event[INTUI_EVENTS_RING];
};

static_assert(sizeof(struct IntuiPage) == 4096, "a connection's page is a page");

/* The client's: how far it has read, shared with intuition read-only. */
struct IntuiAck {
    volatile uint64_t ia_Tail;          /* events read, ever */
};

/* --- intuition.library: the client's side. --- */

constexpr unsigned INTUI_INFLIGHT = 4;      /* presents sent and not yet answered, at most */

/* The connection: the caller's, opened by OpenIntuition. */
struct Intuition {
    struct MsgPort in_Port;
    Handle in_Server;                   /* intuition's task */
    uint32_t in_Width, in_Height;       /* the screen's */
    struct IntuiMsg *in_Call;           /* INFO, OPEN and CLOSE, one at a time */
    struct IntuiMsg *in_Present[INTUI_INFLIGHT];
    uint32_t in_Busy;                   /* a bit for each in_Present in flight */
    int32_t in_Error;                   /* a present refused, not yet returned */
    Handle in_AckArea;                  /* in_Ack's */
    struct IntuiAck *in_Ack;
    const struct IntuiPage *in_Page;
};

/* A window: the caller's, filled in by OpenWindowTagList. Draw the next
 * frame through w_RPort, which is on w_BitMap[w_Back]. */
struct Window {
    struct Intuition *w_Intuition;
    uint32_t w_Id;
    uint32_t w_Buffers;
    int32_t w_LeftEdge, w_TopEdge;      /* as opened; w_Sync's is_LeftEdge, is_TopEdge as dragged */
    uint32_t w_Width, w_Height;         /* its inside's, the buffers' part shown */
    struct BitMap *w_BitMap[INTUI_BUFFERS];
    uint32_t w_Back;                    /* the buffer the next frame is drawn in */
    uint32_t w_Flags;                   /* WFLG_* */
    struct RastPort w_RPort;
    uint64_t w_Frame;                   /* the last presented; 0 before the first */
    const struct IntuiSync *w_Sync;
    int32_t w_BorderLeft, w_BorderTop;  /* the inside's top left, from the chrome's */
};

/* OpenWindowTagList's tags: the RKM's numbers, then Planar's. */
constexpr Tag WA_Dummy = TAG_USER + 99;
constexpr Tag WA_Left = WA_Dummy + 1;           /* 0 */
constexpr Tag WA_Top = WA_Dummy + 2;            /* 0 */
constexpr Tag WA_Width = WA_Dummy + 3;          /* the inside's: the bitmap's */
constexpr Tag WA_Height = WA_Dummy + 4;         /* the inside's: the bitmap's */
constexpr Tag WA_IDCMP = WA_Dummy + 7;          /* IDCMP_* classes: none */
constexpr Tag WA_Flags = WA_Dummy + 8;          /* WFLG_*: none */
constexpr Tag WA_Title = WA_Dummy + 11;         /* const char *: "" */
constexpr Tag WA_BitMap = WA_Dummy + 0x100;     /* struct BitMap *: the first buffer; needed */
constexpr Tag WA_BackBitMap = WA_Dummy + 0x101; /* struct BitMap *: a second; none */

/* WA_Flags: the RKM's bits. */
constexpr uint32_t WFLG_DRAGBAR = 0x00000002;       /* dragged by its title bar */
constexpr uint32_t WFLG_DEPTHGADGET = 0x00000004;
constexpr uint32_t WFLG_CLOSEGADGET = 0x00000008;
constexpr uint32_t WFLG_BORDERLESS = 0x00000800;    /* no chrome: its inside alone */
constexpr uint32_t WFLG_ACTIVATE = 0x00001000;      /* the active window at its first frame */

struct IntuitionIFace {
    struct Interface Data;

    int64_t (*OpenIntuition)(struct Intuition *in, uint64_t deadline);
    void (*CloseIntuition)(struct Intuition *in);
    int64_t (*OpenWindowTagList)(struct Intuition *in, struct Window *w, const struct TagItem *tags);
    int64_t (*CloseWindow)(struct Window *w);
    int64_t (*PresentWindow)(struct Window *w, const struct GfxRect *damage, uint32_t count);
    int64_t (*WaitSafe)(struct Window *w, uint64_t deadline);
    bool (*GetIntuiEvent)(struct Intuition *in, struct IntuiEvent *ev);
    int64_t (*WaitIntuiEvent)(struct Intuition *in, uint64_t deadline);
};

/* The tags as arguments: OpenWindowTags(IIntuition, &in, &w, { WA_Width, 320 }, ...). */
#define OpenWindowTags(iface, in, w, ...) \
    ((iface)->OpenWindowTagList((in), (w), (const struct TagItem[]){ __VA_ARGS__, { TAG_END, 0 } }))

/* lib/intuition's, which the library's table lists. */
int64_t OpenIntuition(struct Intuition *in, uint64_t deadline);
void CloseIntuition(struct Intuition *in);
int64_t OpenWindowTagList(struct Intuition *in, struct Window *w, const struct TagItem *tags);
int64_t CloseWindow(struct Window *w);
int64_t PresentWindow(struct Window *w, const struct GfxRect *damage, uint32_t count);
int64_t WaitSafe(struct Window *w, uint64_t deadline);
bool GetIntuiEvent(struct Intuition *in, struct IntuiEvent *ev);
int64_t WaitIntuiEvent(struct Intuition *in, uint64_t deadline);

#endif /* PLANAR_INTUITION_H */
