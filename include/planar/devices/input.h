/*
 * planar/devices/input.h -- the input class (v5-plan step 4)
 *
 * input (servers/input) is one server for every keyboard and mouse. It
 * publishes "input". Drivers connect to it and send what their devices
 * report: INPUT_ATTACH for each keyboard or mouse they find, then its
 * reports as they come (INPUT_REPORT), raw, in the HID boot protocol's
 * form, and INPUT_DETACH when it goes. When a driver's connection goes,
 * dead or not, its devices go with it, and their keys are let go.
 *
 * input turns the reports into events on a ring, one page, mapped
 * read-only into one client: the Shell now, intuition in V6. The client
 * takes the ring with INPUT_OPEN, and holds it until its connection
 * goes; another meanwhile gets ERR_BUSY. Events are written in turn at
 * ir_Head, which counts every event ever written and is woken with
 * WakeAddr when it moves: a client reads from its own count up to it,
 * and sleeps in WaitAddr. A client more than INPUT_RING behind has lost
 * the oldest; so has one whose event was overwritten while it was
 * copying it, which it sees by reading ir_Head again after.
 *
 * Keys are the HID keyboard page's usages (USB HID Usage Tables §10),
 * with the character each types on a US layout. input repeats a held
 * key that types one, as a keyboard's own typematic would.
 *
 * Its settings (V8 step 7) are anyone's to read and set, as Prefs does:
 * INPUT_GET and INPUT_SET, with an InputPrefs, each field held to its
 * range (a set out of one is refused whole). A mouse's moves are scaled
 * by the pointer's speed, and a fast one (INPUT_ACCEL_FROM or more in a
 * report) by its acceleration too, the fractions kept for the next.
 * input reads its saved settings at start (SYS:Prefs/Env-Archive/
 * input.prefs, planar/prefs.h) once dos is there, unless a client has
 * set them first.
 */
#ifndef PLANAR_DEVICES_INPUT_H
#define PLANAR_DEVICES_INPUT_H

#include <stdint.h>

#include <planar/exec.h>
#include <planar/msg.h>

constexpr unsigned INPUT_REPORT_MAX = 16;  /* bytes of a report: a boot one's 8 and more */

/* InputMsg.im_Op */
enum : uint32_t {
    INPUT_OPEN = 1,         /* a client's: im_Task in; im_Ring out; ERR_BUSY if another has it */
    INPUT_INFO = 2,         /* anyone's: im_Keyboards, im_Mice, im_Held, im_Events out */
    INPUT_GET = 3,          /* anyone's: im_Prefs out (V8 step 7) */
    INPUT_SET = 4,          /* anyone's: im_Prefs in; ERR_BADARG if a field's out of its range */
    INPUT_ATTACH = 0x100,   /* a driver's: its device im_Device is an im_Kind */
    INPUT_DETACH = 0x101,   /* a driver's: im_Device went */
    INPUT_REPORT = 0x102,   /* a driver's: im_Len bytes of im_Device's report, in im_Data */
};

/* InputMsg.im_Kind */
enum : uint32_t { INPUT_KEYBOARD = 1, INPUT_MOUSE = 2 };

/* input's settings, and their ranges. */
struct InputPrefs {
    uint32_t ip_RepeatDelay;            /* ms a key's held before it repeats */
    uint32_t ip_RepeatRate;             /* its repeats a second */
    uint32_t ip_PointerSpeed;           /* % of a mouse's move the pointer moves */
    uint32_t ip_PointerAccel;           /* % more again of a fast one */
};

constexpr uint32_t INPUT_DELAY_MIN = 150, INPUT_DELAY_MAX = 1500, INPUT_DELAY_DEFAULT = 500;
constexpr uint32_t INPUT_RATE_MIN = 2, INPUT_RATE_MAX = 50, INPUT_RATE_DEFAULT = 30;
constexpr uint32_t INPUT_SPEED_MIN = 25, INPUT_SPEED_MAX = 400, INPUT_SPEED_DEFAULT = 100;
constexpr uint32_t INPUT_ACCEL_MIN = 100, INPUT_ACCEL_MAX = 400, INPUT_ACCEL_DEFAULT = 100;
constexpr int32_t INPUT_ACCEL_FROM = 6;     /* |x| + |y| of a report, for it to be fast */

struct InputMsg {
    struct Message im_Msg;
    uint32_t im_Op;
    Handle im_Task;                     /* INPUT_OPEN: the client's own task (FindTask(nullptr)) */
    uint32_t im_Device;                 /* the driver's number for it */
    uint32_t im_Kind;
    uint32_t im_Len;
    uint8_t im_Data[INPUT_REPORT_MAX];
    uint64_t im_Ring;                   /* INPUT_OPEN: the ring, mapped read-only */
    uint32_t im_Keyboards, im_Mice;     /* INPUT_INFO: attached now */
    uint32_t im_Held;                   /* INPUT_INFO: 1 if a client has the ring */
    uint32_t im_Pad;
    uint64_t im_Events;                 /* INPUT_INFO: written to the ring, ever */
    struct InputPrefs im_Prefs;         /* INPUT_GET, INPUT_SET */
};

/* InputEvent.ie_Class */
enum : uint16_t {
    IECLASS_KEY = 1,                    /* ie_Code a key's usage */
    IECLASS_BUTTON = 2,                 /* ie_Code a mouse button: 0 left, 1 right, 2 middle */
    IECLASS_MOVE = 3,                   /* the mouse moved ie_X, ie_Y (down is +y) */
};

/* InputEvent.ie_Flags */
constexpr uint16_t IEF_UP = 1u << 0;        /* let go: a key or button */
constexpr uint16_t IEF_REPEAT = 1u << 1;    /* input's repeat of a held key */

/* InputEvent.ie_Qualifier: the modifier keys held, as a boot report's
 * first byte has them, and Caps Lock. */
constexpr uint16_t IEQ_LCTRL = 1u << 0, IEQ_LSHIFT = 1u << 1, IEQ_LALT = 1u << 2;
constexpr uint16_t IEQ_LGUI = 1u << 3, IEQ_RCTRL = 1u << 4, IEQ_RSHIFT = 1u << 5;
constexpr uint16_t IEQ_RALT = 1u << 6, IEQ_RGUI = 1u << 7;
constexpr uint16_t IEQ_CAPSLOCK = 1u << 8;
constexpr uint16_t IEQ_SHIFT = IEQ_LSHIFT | IEQ_RSHIFT;
constexpr uint16_t IEQ_CTRL = IEQ_LCTRL | IEQ_RCTRL;

/* Some of the keys' usages. */
enum : uint16_t {
    IEKEY_A = 0x04, IEKEY_ENTER = 0x28, IEKEY_ESCAPE = 0x29, IEKEY_BACKSPACE = 0x2a,
    IEKEY_TAB = 0x2b, IEKEY_SPACE = 0x2c, IEKEY_CAPSLOCK = 0x39, IEKEY_DELETE = 0x4c,
    IEKEY_RIGHT = 0x4f, IEKEY_LEFT = 0x50, IEKEY_DOWN = 0x51, IEKEY_UP = 0x52,
    IEKEY_LCTRL = 0xe0,                 /* to 0xe7: the modifiers, as keys */
};

struct InputEvent {
    uint64_t ie_Time;                   /* CurrentTime's, as input had it */
    uint16_t ie_Class;
    uint16_t ie_Code;
    uint16_t ie_Qualifier;
    uint16_t ie_Flags;
    uint32_t ie_Char;                   /* a key going down: what it types, or 0 */
    int32_t ie_X, ie_Y;
    uint32_t ie_Pad;
};

static_assert(sizeof(struct InputEvent) == 32, "an event is 32 bytes");

constexpr unsigned INPUT_RING = 127;        /* events: the ring is a page */

struct InputRing {
    volatile uint64_t ir_Head;          /* events written, ever: the next at ir_Head % INPUT_RING */
    uint64_t ir_Pad[3];
    struct InputEvent ir_Event[INPUT_RING];
};

static_assert(sizeof(struct InputRing) == 4096, "the ring is a page");

#endif /* PLANAR_DEVICES_INPUT_H */
