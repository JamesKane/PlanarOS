/*
 * planar/intuition.h -- windows on the screen (v7-plan steps 3 and 4)
 *
 * intuition (servers/intuition) composes one screen, the Workbench's,
 * on display's plane 1, over the console on plane 0, with the pointer on
 * plane 2. It publishes "intuition". A window's pixels are the client's
 * own (docs §13.1): bitmaps it draws in with gfx.library and shares with
 * intuition read-only. intuition composes from them on the CPU, into
 * two screen buffers of its own shown in turn, and never writes a
 * client's memory.
 *
 * A client presents a frame of a window: which of its buffers to show,
 * what changed since the last frame (up to INTUI_RECTS rectangles; none
 * is the whole window), and the frame's number, above the last. Each
 * window has a line of a page shared read-only with its client, its
 * sync words (§16.3 pillar 4):
 *   is_Disp, the last frame whose pixels are on the screen, latched at
 *            the vblank of is_DispTime;
 *   is_Safe, the last frame intuition has composed: from then on it
 *            reads no buffer that only frames before it showed. A client
 *            with two buffers draws in the other once is_Safe has
 *            reached the frame it last presented.
 * Each is woken with WakeAddr when it moves.
 *
 * The client's side is intuition.library: IIntuition's OpenIntuition,
 * OpenWindowTagList, PresentWindow, WaitSafe and CloseWindow, over the
 * messages below. Its state is the caller's: an Intuition for the
 * connection, a Window for each window.
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

/* IntuiMsg.im_Op */
enum : uint32_t {
    INTUI_INFO = 1,     /* im_Task in; im_Server, the screen's im_Width and im_Height, costs out */
    INTUI_OPEN = 2,     /* im_Task, the window and its buffers in; im_Window, im_Sync out */
    INTUI_CLOSE = 3,    /* im_Window in */
    INTUI_PRESENT = 4,  /* im_Window, im_Index, im_Frame, im_Count rectangles in im_Rect */
};

/* A buffer of a window: memory of the client's, shared with intuition
 * (its INTUI_INFO's im_Server) read-only before INTUI_OPEN. Its top
 * left im_Width by im_Height pixels are the window's. */
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
    uint32_t im_Flags;                  /* none yet */
    int32_t im_Left, im_Top;            /* INTUI_OPEN: where on the screen */
    uint32_t im_Width, im_Height;       /* INTUI_OPEN: the window's; INTUI_INFO: the screen's */
    uint32_t im_Buffers;                /* INTUI_OPEN: in im_Buffer, 1 or 2 */
    uint32_t im_Index;                  /* INTUI_PRESENT: which buffer */
    struct IntuiBuffer im_Buffer[INTUI_BUFFERS];
    char im_Title[INTUI_TITLE];
    uint64_t im_Sync;                   /* INTUI_OPEN: its IntuiSync, mapped read-only */
    uint64_t im_Server;                 /* INTUI_INFO: intuition's task, to share with */
    uint64_t im_Frame;                  /* INTUI_PRESENT: above the last presented */
    uint32_t im_Count;                  /* INTUI_PRESENT: in im_Rect; 0, the whole window */
    uint32_t im_Pad;
    struct GfxRect im_Rect[INTUI_RECTS];    /* the window's own coordinates, inclusive */
    /* INTUI_INFO, for tests and costs: since intuition started. */
    uint64_t im_Frames;                 /* composed */
    uint64_t im_Pixels;                 /* painted composing them */
    uint64_t im_Nanos;                  /* spent composing them */
};

/* A window's sync words: a cache line of its connection's page. */
struct IntuiSync {
    volatile uint64_t is_Disp;
    volatile uint64_t is_DispTime;
    volatile uint64_t is_Safe;
    uint64_t is_Pad[5];
};

static_assert(sizeof(struct IntuiSync) == 64, "a window's sync words are a cache line");

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
};

/* A window: the caller's, filled in by OpenWindowTagList. Draw the next
 * frame through w_RPort, which is on w_BitMap[w_Back]. */
struct Window {
    struct Intuition *w_Intuition;
    uint32_t w_Id;
    uint32_t w_Buffers;
    int32_t w_LeftEdge, w_TopEdge;
    uint32_t w_Width, w_Height;
    struct BitMap *w_BitMap[INTUI_BUFFERS];
    uint32_t w_Back;                    /* the buffer the next frame is drawn in */
    uint32_t w_Pad;
    struct RastPort w_RPort;
    uint64_t w_Frame;                   /* the last presented; 0 before the first */
    const struct IntuiSync *w_Sync;
};

/* OpenWindowTagList's tags: the RKM's numbers, then Planar's. */
constexpr Tag WA_Dummy = TAG_USER + 99;
constexpr Tag WA_Left = WA_Dummy + 1;           /* 0 */
constexpr Tag WA_Top = WA_Dummy + 2;            /* 0 */
constexpr Tag WA_Width = WA_Dummy + 3;          /* the bitmap's */
constexpr Tag WA_Height = WA_Dummy + 4;         /* the bitmap's */
constexpr Tag WA_Title = WA_Dummy + 11;         /* const char *: "" */
constexpr Tag WA_BitMap = WA_Dummy + 0x100;     /* struct BitMap *: the first buffer; needed */
constexpr Tag WA_BackBitMap = WA_Dummy + 0x101; /* struct BitMap *: a second; none */

struct IntuitionIFace {
    struct Interface Data;

    int64_t (*OpenIntuition)(struct Intuition *in, uint64_t deadline);
    void (*CloseIntuition)(struct Intuition *in);
    int64_t (*OpenWindowTagList)(struct Intuition *in, struct Window *w, const struct TagItem *tags);
    int64_t (*CloseWindow)(struct Window *w);
    int64_t (*PresentWindow)(struct Window *w, const struct GfxRect *damage, uint32_t count);
    int64_t (*WaitSafe)(struct Window *w, uint64_t deadline);
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

#endif /* PLANAR_INTUITION_H */
