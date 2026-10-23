/*
 * drv/xhci/xhci.h -- the xHCI driver's own (eXtensible Host Controller
 * Interface 1.2, cited by section)
 */
#ifndef DRV_XHCI_H
#define DRV_XHCI_H

#include <stdint.h>

#include <planar/drv.h>

constexpr unsigned XHC_DEVICES = 12;        /* USB devices a controller drives, hubs too */
constexpr unsigned XHC_HIDS = 2;            /* HID interfaces a device may have */
constexpr unsigned XHC_HUB_PORTS = 15;
constexpr unsigned XHC_REPORTS = 8;         /* a HID endpoint's transfers queued at once */
constexpr uint64_t MS = 1000000;

/* --- Registers (§5). --- */

enum : uint32_t {               /* capability registers */
    CAPLENGTH = 0x00, HCSPARAMS1 = 0x04, HCSPARAMS2 = 0x08, HCCPARAMS1 = 0x10, DBOFF = 0x14,
    RTSOFF = 0x18,
};
enum : uint32_t {               /* operational, from CAPLENGTH */
    USBCMD = 0x00, USBSTS = 0x04, PAGESIZE = 0x08, CRCR = 0x18, DCBAAP = 0x30, CONFIG = 0x38,
    PORTSC = 0x400,             /* port n's at PORTSC + 0x10 * (n - 1) */
};
enum : uint32_t {               /* interrupter 0's, from RTSOFF */
    IMAN = 0x20, IMOD = 0x24, ERSTSZ = 0x28, ERSTBA = 0x30, ERDP = 0x38,
};
enum : uint32_t { CMD_RS = 1u << 0, CMD_HCRST = 1u << 1, CMD_INTE = 1u << 2 };
enum : uint32_t { STS_HCH = 1u << 0, STS_HSE = 1u << 2, STS_EINT = 1u << 3, STS_PCD = 1u << 4,
                  STS_CNR = 1u << 11 };
enum : uint32_t { IMAN_IP = 1u << 0, IMAN_IE = 1u << 1 };
constexpr uint64_t ERDP_EHB = 1u << 3;
constexpr uint32_t HCC_AC64 = 1u << 0, HCC_CSZ = 1u << 2, HCC_PPC = 1u << 3;

enum : uint32_t {               /* PORTSC (§5.4.8) */
    PS_CCS = 1u << 0, PS_PED = 1u << 1, PS_PR = 1u << 4, PS_PP = 1u << 9,
    PS_CSC = 1u << 17, PS_PEC = 1u << 18, PS_WRC = 1u << 19, PS_OCC = 1u << 20,
    PS_PRC = 1u << 21, PS_PLC = 1u << 22, PS_CEC = 1u << 23,
    PS_CHANGES = 0x7fu << 17,
    /* What a write keeps as it is: the read-only bits and those that
     * hold their value (Linux's xhci_port_state_to_neutral). */
    PS_KEEP = 0x1u | 0x8u | 0x3c00u | 0x40000000u | 0x01000000u | 0x1e0u | 0x200u | 0xc000u |
              0x0e000000u,
};

static inline uint32_t ps_speed(uint32_t ps)
{
    return ps >> 10 & 0xf;
}

/* Speeds, as PORTSC and the slot context have them (§7.2.2.1.1's defaults). */
enum : uint8_t { SPEED_FULL = 1, SPEED_LOW = 2, SPEED_HIGH = 3, SPEED_SUPER = 4 };

/* --- TRBs (§6.4). --- */

struct trb {
    uint64_t ptr;
    uint32_t status;
    uint32_t control;
};

static_assert(sizeof(struct trb) == 16, "a TRB is 16 bytes");

enum : uint32_t {
    TRB_C = 1u << 0, TRB_TC = 1u << 1, TRB_ENT = 1u << 1, TRB_ISP = 1u << 2, TRB_CH = 1u << 4,
    TRB_IOC = 1u << 5, TRB_IDT = 1u << 6, TRB_BSR = 1u << 9, TRB_DIR_IN = 1u << 16,
};
enum : uint32_t {
    T_NORMAL = 1, T_SETUP = 2, T_DATA = 3, T_STATUS = 4, T_LINK = 6,
    T_ENABLE_SLOT = 9, T_DISABLE_SLOT = 10, T_ADDRESS = 11, T_CONFIGURE = 12, T_EVALUATE = 13,
    T_RESET_EP = 14, T_STOP_EP = 15, T_SET_DEQ = 16, T_NOOP_CMD = 23,
    T_TRANSFER_EV = 32, T_COMMAND_EV = 33, T_PORT_EV = 34, T_HC_EV = 37,
};
enum : uint32_t { CC_SUCCESS = 1, CC_TRANSACTION = 4, CC_STALL = 6, CC_SHORT = 13 };

static inline uint32_t trb_type(uint32_t t)
{
    return t << 10;
}

/* A ring of TRBs (§4.9): the last a link back to the first. */
struct ring {
    struct trb *t;
    uint32_t n;                     /* TRBs, the link among them */
    uint32_t enq;
    uint32_t cycle;
};

/* --- Contexts (§6.2), each 32 or 64 bytes (HCC_CSZ). --- */

enum : uint8_t { EP_CONTROL = 4, EP_INT_IN = 7 };

/* --- The controller and its devices. --- */

struct xdev;

/* Where a mouse's input report has its buttons, X and Y (hid.c): bit
 * offsets after the report ID, which leads it if 'ids'. */
struct hid_layout {
    bool ids, has_x, has_y;
    uint8_t id;
    uint8_t x_bits, y_bits;
    uint16_t x_at, y_at;
    uint16_t button_at[3];          /* bit + 1; 0: none */
};

struct hid {
    uint8_t kind;                   /* INPUT_KEYBOARD, INPUT_MOUSE; 0 none */
    uint8_t iface, dci;
    uint16_t mps;
    uint8_t interval;               /* the endpoint's, as the context has it */
    struct ring ring;
    uint8_t *buf;                   /* XHC_REPORTS reports' of 64 bytes */
    bool attached;                  /* told to input */
    bool halted;                    /* by an error: to be reset */
    uint32_t halts;                 /* ever, said at the first few and then every 100th */
    uint16_t report_len;            /* its report descriptor's bytes (its HID descriptor's) */
    bool by_layout;                 /* a mouse read in the report protocol, through 'layout' */
    struct hid_layout layout;
};

struct xdev {
    bool used, hub;
    uint8_t slot, speed, port;      /* port: on its parent hub, or the root's */
    uint8_t root_port, depth;
    uint8_t tt_slot, tt_port;       /* the high-speed hub whose TT it's behind, and its port */
    uint8_t parent;                 /* its hub's index in xhc.dev, or 0xff on a root port */
    uint8_t nports;                 /* a hub's */
    uint32_t route;
    uint16_t mps0;
    uint8_t *page;                  /* three DMA pages: rings and buffers; input, output contexts */
    uint8_t *in, *out;
    struct ring ep0;
    struct hid hid[XHC_HIDS];
    uint32_t hub_ports;             /* a hub's ports with something on them */
    uint64_t hub_poll;              /* when to look at a hub's ports next */
};

struct xhc {
    struct drv_dev *drv;
    unsigned index;                 /* among the driver's controllers */
    volatile uint8_t *cap, *op, *rt, *db;
    volatile uint8_t *phy2;         /* the K1's USB 2 PHY (dwc3.c); else nullptr */
    uint32_t ctx;                   /* 32 or 64 */
    uint32_t max_slots, max_ports;
    bool irq, ac64;
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *ev;                 /* the event ring: one segment */
    uint32_t ev_n, ev_deq, ev_cycle;
    uint8_t usb2_lo, usb2_hi;       /* the root ports that are USB 2's (Supported Protocol) */
    uint32_t pending;               /* root ports to look at */
    struct xdev dev[XHC_DEVICES];
    uint8_t *pool;                  /* the devices' pages */
    /* The command or transfer being waited for, and how it came back. */
    uint64_t wait_trb;
    uint32_t wait_slot;             /* a control transfer's: its errors end the wait too */
    bool wait_done;
    uint32_t wait_code, got_slot;   /* got_slot: Enable Slot's answer */
};

/* hid.c */
bool hid_layout(const uint8_t *d, uint32_t n, struct hid_layout *l);
bool hid_boot(const struct hid_layout *l, const uint8_t *r, uint32_t len, uint8_t out[3]);

/* hc.c */
uint32_t xr(const volatile uint8_t *base, uint32_t off);
void xw(volatile uint8_t *base, uint32_t off, uint32_t v);
void xw64(volatile uint8_t *base, uint32_t off, uint64_t v);
void *xdma(struct drv *d, uint64_t size);
void xbarrier(void);
void nap(uint64_t ms);
bool ring_new(struct ring *r, struct trb *t, uint32_t n);
struct trb *ring_put(struct ring *r, uint64_t ptr, uint32_t status, uint32_t control);
int hc_start(struct drv *d, struct xhc *c);
unsigned hc_events(struct drv *d, struct xhc *c);   /* transfer events it handled */
int hc_command(struct drv *d, struct xhc *c, uint64_t ptr, uint32_t status, uint32_t control,
               uint32_t *slot);
void hc_recover(struct drv *d, struct xhc *c, struct xdev *x, uint8_t dci, struct ring *r);
int hc_control(struct drv *d, struct xhc *c, struct xdev *x, uint8_t type, uint8_t req,
               uint16_t value, uint16_t index, uint16_t len, void *data);
void hc_ring_ep(struct xhc *c, uint8_t slot, uint8_t dci);
uint32_t *ctx_in(struct xhc *c, struct xdev *x, unsigned i);
uint32_t *ctx_out(struct xhc *c, struct xdev *x, unsigned i);

/* usb.c */
void usb_port(struct drv *d, struct xhc *c, unsigned port);
void usb_transfer(struct drv *d, struct xhc *c, uint32_t slot, uint32_t dci, uint32_t code,
                  uint32_t residual, uint64_t trb);
void usb_hubs(struct drv *d, struct xhc *c);
uint64_t usb_next_poll(struct xhc *c);
void usb_attach_all(struct xhc *c);

/* main.c: to input. */
void input_attach(struct xhc *c, struct xdev *x, struct hid *h);
void input_detach(struct xhc *c, struct xdev *x, struct hid *h);
void input_report(struct xhc *c, struct xdev *x, struct hid *h, const uint8_t *data, uint32_t len);

/* main.c: lines for the log. */
struct xline {
    char t[160];
    unsigned n;
};
void lput(struct xline *l, const char *s);
void lnum(struct xline *l, uint64_t v, unsigned base);
void xsay(struct xhc *c, struct xline *l);
void xlog(struct xhc *c, const char *text);

#endif /* DRV_XHCI_H */
