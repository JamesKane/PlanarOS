/*
 * planar/drv.h -- drv.library (lib/drv), and what it says to hwdb
 *
 * docs §11.7. hwdb starts a driver process with one argument, a token,
 * after granting it its leases. The library connects to hwdb's port,
 * asks for its devices with the token (HWDB_BIND), maps the leases, and
 * runs the driver's callbacks: Bind for the devices, Start to publish its
 * class ports, then a loop that hands each interrupt to Interrupt and
 * each request on a class port to Request. When Start has run it tells
 * hwdb which devices it took and what it published (HWDB_STARTED); a
 * driver that took none exits, and hwdb counts them declined.
 *
 * hwdb never waits for a driver (§11.6 D6): it answers these messages
 * and otherwise only watches the process. Once started, the library
 * leaves an HWDB_STOP with hwdb, which answers it to ask the driver to
 * stop (V6 step 7, §11 D1): by hm_Deadline the driver turns its devices
 * off, gives back its DMA and exits 0. One that doesn't is killed then,
 * and the kill path, which has to be right anyway, does the rest.
 */
#ifndef PLANAR_DRV_H
#define PLANAR_DRV_H

#include <stdint.h>

#include <planar/lease.h>
#include <planar/msg.h>

constexpr unsigned DRV_DEVICES = 32;        /* hwdb's HW_LEASE_NODES */
constexpr unsigned DRV_RANGES = 4;          /* hwdb's HW_LEASE_RANGES */
constexpr unsigned DRV_PORTS = 2;           /* class ports a device publishes */
constexpr uint32_t DRV_NOLINE = UINT32_MAX;

/* --- The protocol (hwdb's port "hwdb"). --- */

enum : uint32_t {
    HWDB_BIND = 1,          /* hm_Token in; the rest out */
    HWDB_STARTED = 2,       /* hm_Token, hm_Accepted, hm_Ports in */
    HWDB_PCI_HOST = 3,      /* pci's: hm_Host.ph_Device in; the rest of hm_Host out */
    HWDB_PCI_FUNCTION = 4,  /* pci's: hm_Function in, a function it found */
    HWDB_UNRESPONSIVE = 5,  /* anyone's: hm_PortName's server missed a deadline */
    HWDB_STOP = 6,          /* a started driver's: answered when it's to stop, by hm_Deadline */
    HWDB_KILL_CLIENT = 7,   /* intuition's: its client on hm_Conn isn't answering; end it (V8 step 4) */
    HWDB_NEW_SHELL = 8,     /* intuition's: the system menu's Shell; a console window started (V8 step 5) */
    HWDB_NEW_PREFS = 9,     /* the Workbench's: Tools' Prefs started, unless it's running (V8 step 7) */
    HWDB_TEST_KILL = 10,    /* the desktop test's: hm_Token's program killed; HWDB_TEST only (V8 step 8) */
};

/* HWDB_TEST_KILL's hm_Token: what's killed. */
enum : uint32_t {
    HWDB_KILL_WORKBENCH = 1, HWDB_KILL_PREFS, HWDB_KILL_SHELL, HWDB_KILL_INTUITION, HWDB_KILL_INPUT,
};

constexpr uint64_t DRV_STOP_MS = 1000;      /* a driver's time to stop, once asked */
constexpr uint64_t DRV_HWDB_MS = 5000;      /* hwdb's answers, which come at once */

/* hm_Flags, from hwdb: what the kill test asks of this start. */
constexpr uint32_t DRVF_CRASH = 1u << 0;    /* fault in Bind (the give-up test) */
constexpr uint32_t DRVF_AGAIN = 1u << 1;    /* a process of this unit ran before: the device is as it left it */

struct DrvRange {
    Handle dr_Lease;                /* a kernel lease, granted to this process */
    uint32_t dr_Pad;
    uint64_t dr_Base, dr_Size;
};

struct DrvDevice {
    uint64_t dd_Base, dd_Size;      /* its registers: physical, within a range */
    uint32_t dd_Line;               /* index into the first range's lines; DRV_NOLINE */
    uint32_t dd_Unit;               /* among this driver's devices, fixed at match */
    char dd_Key[40];                /* the key it matched: its compatible or _HID */
    /* A PCI function's MSI-X: dd_NLines vectors, vector i on line dd_Line
     * + i, each written as event i to dd_MsiDoorbell. The driver fills
     * the table at dd_MsixTable (in its registers) and unmasks it; pci
     * has enabled MSI-X. With no MSI-X (dd_MsiDoorbell 0), dd_NLines is
     * how many of the device's own lines there are from dd_Line on: the
     * library binds them all only if the driver says so (every_line). */
    uint64_t dd_MsiDoorbell;
    uint64_t dd_MsixTable;
    uint32_t dd_NLines;
    uint32_t dd_Pad;
    /* Its registers' cells, when the description gives them more than an
     * address: a framebuffer's width, height, pixels a line and format. */
    uint32_t dd_Cells[4];
};

/* --- PCI (drv/pci): the host bridge, and the functions found behind it. --- */

constexpr unsigned PCI_WINDOWS = 8;
constexpr unsigned PCI_BARS = 6;

/* PciWindow.pw_Flags and PciBar.pb_Flags */
constexpr uint32_t PCI_MEM = 1u << 0;           /* memory space; a window may be I/O */
constexpr uint32_t PCI_64 = 1u << 1;            /* above 4 GiB on the bus, or a 64-bit BAR */
constexpr uint32_t PCI_PREFETCH = 1u << 2;

struct PciWindow {
    uint64_t pw_Base, pw_Size;      /* the CPU's addresses */
    uint64_t pw_Bus;                /* where pw_Base is on the bus */
    uint32_t pw_Flags, pw_Pad;
};

struct PciHost {
    uint32_t ph_Device;             /* in: which of the driver's devices */
    uint32_t ph_Segment, ph_FirstBus, ph_LastBus;
    uint32_t ph_NWindows;
    uint32_t ph_Msi;                /* its functions' MSIs reach an ITS */
    struct PciWindow ph_Window[PCI_WINDOWS];
};

struct PciBar {
    uint64_t pb_Base, pb_Size;      /* the CPU's address (identity on these buses); 0 unused */
    uint32_t pb_Flags, pb_Pad;
};

struct PciFunction {
    uint32_t pf_Device;             /* the host bridge it's behind, as ph_Device */
    uint32_t pf_Rid;                /* bus << 8 | device << 3 | function */
    uint32_t pf_Vendor, pf_Id;      /* its vendor and device IDs */
    uint32_t pf_Class;              /* class << 16 | subclass << 8 | programming interface */
    uint32_t pf_Revision;
    uint32_t pf_MsixVectors;        /* its MSI-X table's size; 0 if none */
    uint32_t pf_MsixBar;            /* the table's BAR, and its offset there */
    uint64_t pf_MsixOffset;
    struct PciBar pf_Bar[PCI_BARS]; /* a 64-bit BAR is the first of its pair */
};

struct HwdbMsg {
    struct Message hm_Msg;
    uint32_t hm_Op;
    uint32_t hm_Token;
    union {
        struct {                    /* HWDB_BIND and HWDB_STARTED */
            uint32_t hm_Flags;
            uint32_t hm_NRanges;
            uint32_t hm_NDevices;
            uint32_t hm_Accepted;   /* HWDB_STARTED: a bit per device */
            struct DrvRange hm_Range[DRV_RANGES];
            struct DrvDevice hm_Device[DRV_DEVICES];
            char hm_Ports[DRV_DEVICES][16]; /* HWDB_STARTED: each device's class port, or "" */
        };
        struct PciHost hm_Host;
        struct PciFunction hm_Function;
        uint64_t hm_Deadline;       /* HWDB_STOP's answer: CurrentTime's clock */
        char hm_PortName[32];       /* HWDB_UNRESPONSIVE */
        uint64_t hm_Conn;           /* HWDB_KILL_CLIENT: intuition's handle for the connection */
    };
};

/* --- A request on a class port (the step 3 drivers' one class). --- */

enum : uint32_t {
    DRV_READ = 1,                   /* the device's value: the time, random bytes */
    DRV_WRITE = 2,                  /* dr_Len bytes of dr_Data to it: text, to a console */
};

struct DrvRequest {
    struct Message dr_Msg;
    uint32_t dr_Op;
    uint32_t dr_Len;                /* bytes of dr_Data wanted; set to what came */
    uint8_t dr_Data[64];
};

/* --- The library. --- */

/* Bind's answers. */
constexpr int DRV_OK = 0;
constexpr int DRV_DECLINE = 1;      /* not a device this driver drives (an empty slot) */

constexpr unsigned DRV_CLIENTS = 8; /* connections per class port */

struct drv;

struct drv_dev {
    const struct DrvDevice *info;
    volatile uint8_t *regs;         /* mapped: dd_Base */
    bool accepted;                  /* Bind took it */
    bool line_bound;
    Handle port[DRV_PORTS];         /* its class ports, in the order published; 0 */
    char port_name[DRV_PORTS][16];  /* hwdb is told the first */
    void *priv;                     /* the driver's */
};

struct drv_ops {
    const char *name;
    /* Each device in turn: DRV_OK to take it, DRV_DECLINE, or an error
     * (negative), which ends the process. */
    int (*bind)(struct drv *d, struct drv_dev *dev);
    /* After Bind, for each device taken: publish its class port. */
    int (*start)(struct drv *d, struct drv_dev *dev);
    void (*interrupt)(struct drv *d, struct drv_dev *dev);
    /* One request; its reply goes back when this returns. */
    void (*request)(struct drv *d, struct drv_dev *dev, struct DrvRequest *r);
    /* A class's own messages, in place of request (block's): each as it
     * comes, from client 'c' (a number that stays the client's), in the
     * connection's memory, at least 'message_size' bytes of it. The
     * driver answers each with drv_reply, now or later. */
    void (*message)(struct drv *d, struct drv_dev *dev, unsigned c, struct Message *m);
    uint32_t message_size;
    /* A device it polls: called each time round the loop, which doesn't
     * sleep while this says work is outstanding. */
    bool (*poll)(struct drv *d);
    /* When poll should be called even with nothing come in (a command's
     * deadline, in case its interrupt doesn't), as CurrentTime() goes; 0
     * if never. The loop sleeps until then at most. */
    uint64_t (*deadline)(struct drv *d);
    /* Signals of the driver's own that wake the loop (and so poll): a
     * connection of its own to a server, which is signalled when the
     * server goes, and a watch's (WatchPort), for its coming back. Bits
     * 20 to 31. */
    uint64_t signals;
    /* Client 'c' went (its connection closed, or it died): what it held
     * of the driver's is to be let go (§11.5, per-connection state). It
     * may still have messages unanswered; drv_reply them as before. */
    void (*gone)(struct drv *d, struct drv_dev *dev, unsigned c);
    /* Bind every line a device has (dd_NLines), not only its first: a
     * device with a line per source (the G720's job, MMU and GPU). */
    bool every_line;
    /* hwdb asked it to stop (V6 step 7): for each device taken, turn it
     * off so it touches no more memory, and FreeArea its DMA Areas, by
     * DRV_STOP_MS. The process then exits 0, and hwdb finds its leases
     * idle, nothing quarantined. Without one it only exits: the kernel
     * quarantines its DMA, as when it's killed, and hwdb's stop method
     * stops the device. */
    void (*stop)(struct drv *d, struct drv_dev *dev);
};

struct drv {
    const struct drv_ops *ops;
    uint32_t token;                 /* hwdb's, for messages a bus driver sends it */
    struct HwdbMsg info;            /* hwdb's answer, copied */
    struct drv_dev dev[DRV_DEVICES];
    unsigned ndev;
    struct MsgPort hwdb;
};

/* The driver's main: runs the protocol and the loop; never returns. */
[[noreturn]] void drv_main(uint64_t token, const struct drv_ops *ops);

/* Answer client c's message. A client that went keeps its connection
 * until the driver has answered all it sent: until then its arena
 * holds buffers the device may be using. */
void drv_reply(struct drv *d, unsigned c, struct Message *m);

/* The connection's heap, where client c's buffers must lie. */
bool drv_in_heap(const struct drv *d, unsigned c, uint64_t addr, uint64_t size);

/* For Start: publish 'dev's class port, named class.unit ("rtc.0"). A
 * device may publish DRV_PORTS, each a class of its own (display's
 * "con.0" and "display.0"); their clients' messages all come to the
 * same callbacks. ERR_FULL past that. */
int64_t drv_publish(struct drv *d, struct drv_dev *dev, const char *cls);
/* The same, named 'name' as it is: a server there is one of ("power"). */
int64_t drv_publish_as(struct drv *d, struct drv_dev *dev, const char *name);

/* Bind's helpers: the first range's lease, which carries the lines; wait
 * for 'dev's line (or 'mask', or 'ms' milliseconds; 0: no limit),
 * acknowledging it once it came. The signals that came. */
Handle drv_lease(const struct drv *d);
uint64_t drv_wait_irq(struct drv *d, struct drv_dev *dev, uint64_t ms);

void drv_log(const struct drv *d, const struct drv_dev *dev, const char *text);

/* DMA memory and the caches, for a device that doesn't snoop them (the
 * K1's): clean what the device will read before it does; flush what it
 * will write before it starts and again before reading it. Each ISA's
 * (arch/<arch>/user/cache.c); orders only where the machine is coherent. */
void drv_dma_clean(const void *p, uint64_t n);
void drv_dma_flush(const void *p, uint64_t n);
/* Orders memory a device reads or writes, uncached Areas' too, against
 * its registers: what was written before a doorbell, what the device
 * wrote before it's read. More than a compiler's fence on both ISAs. */
void drv_io_barrier(void);

/* For a bus driver's Bind: a message for hwdb, the size of any (one per
 * driver, reused), and DoMsg with it; 0, or the error hwdb or the
 * connection gave. */
struct HwdbMsg *drv_hwdb_msg(struct drv *d, uint32_t op);
int64_t drv_hwdb(struct drv *d, struct HwdbMsg *m);

#endif /* PLANAR_DRV_H */
