/*
 * planar/devices/display.h -- the display class (docs §16.3, v5-plan step 3)
 *
 * display drives one output, on the mode the firmware left: it never sets
 * one. It publishes two ports. "con.0" is the text console (the console
 * class, DrvRequest's DRV_WRITE), drawn on the output's plane 0. Its
 * second, "display.0", gives the other planes to clients.
 *
 * A plane shows a buffer, a rectangle on the screen and a format. The
 * server allocates every buffer (DISPLAY_ALLOC), where its controller can
 * reach it, and maps it into the client: a buffer is the server's, so a
 * client that dies can't take it from under a scanout. A client's commit
 * (DISPLAY_COMMIT) sets some planes at once, and is latched at a vblank:
 * the answer comes at once, with the commit's number, and the client
 * waits for that to reach ds_Disp in the output's sync page (WaitAddr).
 * A plane is the client's that first commits to it, until it commits the
 * plane off, or goes.
 *
 * When a client goes, dead or not, its planes go off at the next vblank,
 * and its buffers are freed once that has latched: never while the
 * controller may still read them (§16.6). DISPLAY_FREE frees one the same
 * way.
 *
 * The sync page is one page, mapped read-only into each client (§16.3
 * pillar 4): ds_Vblank counts vblanks, at ds_Time (CurrentTime's); ds_Disp
 * is the last commit on the screen; ds_Safe the last whose predecessors'
 * buffers are no longer read, which a buffer the client draws into next
 * waits for. On the controllers display drives they latch together.
 * Each is woken with WakeAddr when it moves.
 */
#ifndef PLANAR_DEVICES_DISPLAY_H
#define PLANAR_DEVICES_DISPLAY_H

#include <stdint.h>

#include <planar/exec.h>
#include <planar/msg.h>

constexpr unsigned DISPLAY_PLANES = 8;      /* a commit's, and an output's, at most */

/* DisplayMsg.dm_Op: above the DrvRequest ops "con.0" takes. */
enum : uint32_t {
    DISPLAY_INFO = 0x100,       /* dm_Task in; the output, its planes, dm_Sync, dm_Buffers out */
    DISPLAY_ALLOC = 0x101,      /* dm_Task, dm_Width, dm_Height, dm_Format in; dm_Buffer, dm_Address, dm_Stride out */
    DISPLAY_FREE = 0x102,       /* dm_Buffer in */
    DISPLAY_COMMIT = 0x103,     /* dm_Count planes in dm_Set; dm_Seq out */
};

/* Formats: 32 bits a pixel, blue in the low byte. */
enum : uint32_t {
    DISPLAY_XRGB8888 = 1,       /* opaque */
    DISPLAY_ARGB8888 = 2,       /* blended over the planes below, not premultiplied */
};

/* DisplayPlaneInfo.di_Flags */
constexpr uint32_t DISPLAYP_CONSOLE = 1u << 0;  /* the server's: the text console's */
constexpr uint32_t DISPLAYP_CURSOR = 1u << 1;   /* on top of the rest */

struct DisplayPlaneInfo {
    uint32_t di_Flags;
    uint32_t di_Formats;                /* 1 << DISPLAY_XRGB8888, ... */
    uint32_t di_MaxWidth, di_MaxHeight;
};

/* One plane of a commit. Planes stack by number, 0 at the bottom. */
struct DisplayPlane {
    uint32_t dp_Plane;
    uint32_t dp_Buffer;                 /* 0: off */
    int32_t dp_X, dp_Y;                 /* where on the screen the buffer's top left goes */
};

struct DisplayMsg {
    struct Message dm_Msg;
    uint32_t dm_Op;
    Handle dm_Task;                     /* the client's own task (FindTask(nullptr)) */
    uint32_t dm_Width, dm_Height;       /* the output's; a buffer's */
    uint32_t dm_RefreshMilliHz;         /* DISPLAY_INFO: 0 if not known */
    uint32_t dm_Format;
    uint32_t dm_Buffer;
    uint32_t dm_Stride;                 /* bytes a line */
    uint64_t dm_Address;                /* the buffer, mapped into the client */
    uint64_t dm_Sync;                   /* DISPLAY_INFO: the sync page, mapped read-only */
    uint64_t dm_Seq;                    /* DISPLAY_COMMIT: the commit's number */
    uint32_t dm_Count;                  /* planes: of the output, or in dm_Set */
    uint32_t dm_Buffers;                /* DISPLAY_INFO: buffers allocated, every client's */
    union {
        struct DisplayPlaneInfo dm_Plane[DISPLAY_PLANES];   /* DISPLAY_INFO */
        struct DisplayPlane dm_Set[DISPLAY_PLANES];         /* DISPLAY_COMMIT */
    };
};

struct DisplaySync {
    volatile uint64_t ds_Vblank;
    volatile uint64_t ds_Time;
    volatile uint64_t ds_Disp;
    volatile uint64_t ds_Safe;
};

#endif /* PLANAR_DEVICES_DISPLAY_H */
