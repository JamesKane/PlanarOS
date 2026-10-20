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
 * hwdb never waits for a driver (§11.6 D6): it answers these two
 * messages and otherwise only watches the process. There is no STOP
 * message yet: hwdb unbinds by killing (§11.4), and the kill path is the
 * one that has to be right.
 */
#ifndef PLANAR_DRV_H
#define PLANAR_DRV_H

#include <stdint.h>

#include <planar/lease.h>
#include <planar/msg.h>

constexpr unsigned DRV_DEVICES = 32;        /* hwdb's HW_LEASE_NODES */
constexpr unsigned DRV_RANGES = 4;          /* hwdb's HW_LEASE_RANGES */
constexpr uint32_t DRV_NOLINE = UINT32_MAX;

/* --- The protocol (hwdb's port "hwdb"). --- */

enum : uint32_t {
    HWDB_BIND = 1,          /* hm_Token in; the rest out */
    HWDB_STARTED = 2,       /* hm_Token, hm_Accepted, hm_Ports in */
};

/* hm_Flags, from hwdb: what the kill test asks of this start. */
constexpr uint32_t DRVF_CRASH = 1u << 0;    /* fault in Bind (the give-up test) */

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
};

struct HwdbMsg {
    struct Message hm_Msg;
    uint32_t hm_Op;
    uint32_t hm_Token;
    uint32_t hm_Flags;
    uint32_t hm_NRanges;
    uint32_t hm_NDevices;
    uint32_t hm_Accepted;           /* HWDB_STARTED: a bit per device */
    struct DrvRange hm_Range[DRV_RANGES];
    struct DrvDevice hm_Device[DRV_DEVICES];
    char hm_Ports[DRV_DEVICES][16]; /* HWDB_STARTED: each device's class port, or "" */
};

/* --- A request on a class port (the step 3 drivers' one class). --- */

enum : uint32_t {
    DRV_READ = 1,                   /* the device's value: the time, random bytes */
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
    Handle port;                    /* its class port, or 0 */
    char port_name[16];
    struct MsgPort clients[DRV_CLIENTS];
    unsigned nclients;
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
};

struct drv {
    const struct drv_ops *ops;
    struct HwdbMsg info;            /* hwdb's answer, copied */
    struct drv_dev dev[DRV_DEVICES];
    unsigned ndev;
    struct MsgPort hwdb;
};

/* The driver's main: runs the protocol and the loop; never returns. */
[[noreturn]] void drv_main(uint64_t token, const struct drv_ops *ops);

/* For Start: publish 'dev's class port, named class.unit ("rtc.0"). */
int64_t drv_publish(struct drv *d, struct drv_dev *dev, const char *cls);

/* Bind's helpers: the first range's lease, which carries the lines; wait
 * for 'dev's line (or 'mask', or 'ms' milliseconds; 0: no limit),
 * acknowledging it once it came. The signals that came. */
Handle drv_lease(const struct drv *d);
uint64_t drv_wait_irq(struct drv *d, struct drv_dev *dev, uint64_t ms);

void drv_log(const struct drv *d, const struct drv_dev *dev, const char *text);

#endif /* PLANAR_DRV_H */
