/*
 * drv/xhci/usb.c -- devices: addressed, hubs walked, keyboards and mice
 * read (USB 2.0 ch. 9 and 11; HID 1.11; xHCI 1.2 §4.3)
 *
 * A device is addressed as it's found: a slot, its address, its
 * descriptors, its first configuration. A hub's ports are then powered
 * and looked at every 250 ms (its status endpoint isn't read). A
 * full- or low-speed device behind a high-speed hub is reached through
 * that hub's transaction translator, which its slot context names
 * (§4.3.3, single TT). A HID interface that speaks the boot protocol
 * (subclass 1) is put in it, and its interrupt endpoint read for ever,
 * each report sent to input as it is.
 *
 * Only USB 2's ports are walked: SuperSpeed devices (the RV2's hub's
 * USB 3 half, a USB 3 stick) are left alone. Keyboards and mice are
 * never SuperSpeed, and a USB 3 hub has a USB 2 hub inside it.
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/syscalls.h>

#include "xhci.h"

void *memset(void *dst, int c, size_t n);

constexpr uint32_t PAGE = 4096;
constexpr uint32_t EP0_TRBS = 64, HID_TRBS = 32;
constexpr uint32_t CFG_MAX = 1024;
constexpr uint64_t HUB_EVERY = 250 * MS;

enum : uint8_t { GET_STATUS = 0, CLEAR_FEATURE = 1, SET_FEATURE = 3, GET_DESCRIPTOR = 6,
                 SET_CONFIGURATION = 9, SET_IDLE = 0x0a, SET_PROTOCOL = 0x0b };
enum : uint16_t { F_PORT_RESET = 4, F_PORT_POWER = 8, F_C_CONNECTION = 16, F_C_ENABLE = 17,
                  F_C_SUSPEND = 18, F_C_OVER_CURRENT = 19, F_C_RESET = 20 };

/* Where it is, as "port 1.3": its root port, then each hub's. */
static void path(struct xhc *c, const struct xdev *x, struct xline *l)
{
    if (x->parent != 0xff) {
        path(c, &c->dev[x->parent], l);
        lput(l, ".");
    } else {
        lput(l, "port ");
    }
    lnum(l, x->port, 10);
}

static void say_dev(struct xhc *c, const struct xdev *x, const char *what)
{
    struct xline l = { 0 };

    path(c, x, &l);
    lput(&l, ": ");
    lput(&l, what);
    xsay(c, &l);
}

/* --- Going. --- */

static void gone(struct drv *d, struct xhc *c, unsigned i)
{
    struct xdev *x = &c->dev[i];

    for (unsigned k = 0; k < XHC_DEVICES; k++)
        if (c->dev[k].used && c->dev[k].parent == i)
            gone(d, c, k);
    for (unsigned h = 0; h < XHC_HIDS; h++)
        if (x->hid[h].attached)
            input_detach(c, x, &x->hid[h]);
    if (x->slot) {
        hc_command(d, c, 0, 0, trb_type(T_DISABLE_SLOT) | (uint32_t)x->slot << 24, nullptr);
        c->dcbaa[x->slot] = 0;
    }
    say_dev(c, x, "gone");
    x->used = false;
}

static int child(struct xhc *c, unsigned parent, unsigned port)
{
    for (unsigned k = 0; k < XHC_DEVICES; k++)
        if (c->dev[k].used && c->dev[k].parent == parent && c->dev[k].port == port)
            return (int)k;
    return -1;
}

/* --- Endpoints. --- */

/* The input context's slot context, from the output's, with only 'add'
 * to be added (§4.6.6). */
static uint32_t *slot_from_out(struct xhc *c, struct xdev *x, uint32_t add)
{
    uint32_t *ctl = ctx_in(c, x, 0), *s = ctx_in(c, x, 1), *o = ctx_out(c, x, 0);

    memset(x->in, 0, PAGE);
    ctl[1] = add;
    for (unsigned k = 0; k < 4; k++)
        s[k] = o[k];
    s[3] = 0;
    return s;
}

/* The xHCI interval: 2^n microframes. A full- or low-speed endpoint
 * gives frames (1 to 255 ms); a high-speed one 2^(b-1) microframes. */
static uint8_t interval(uint8_t speed, uint8_t b)
{
    uint8_t n = 3;

    if (speed == SPEED_HIGH)
        return b >= 1 && b <= 16 ? (uint8_t)(b - 1) : 3;
    while (n < 10 && (1u << (n + 1)) <= 8u * b)
        n++;
    return n;
}

/* Every report buffer a transfer, so the controller goes on polling the
 * device while the driver is late: a keyboard sends a report only when
 * something changes, and a key down and up again between two polls
 * would be lost. */
static void hid_queue(struct xhc *c, struct xdev *x, struct hid *h)
{
    for (unsigned i = 0; i < XHC_REPORTS; i++)
        ring_put(&h->ring, (uint64_t)(h->buf + 64 * i), h->mps,
                 trb_type(T_NORMAL) | TRB_IOC | TRB_ISP);
    hc_ring_ep(c, x->slot, h->dci);
}

/* Its HID interfaces' endpoints configured, in the boot protocol, and
 * read. */
static bool hid_start(struct drv *d, struct xhc *c, struct xdev *x)
{
    uint32_t add = 1, top = 1, *s;

    for (unsigned k = 0; k < XHC_HIDS; k++)
        if (x->hid[k].kind) {
            add |= 1u << x->hid[k].dci;
            if (x->hid[k].dci > top)
                top = x->hid[k].dci;
        }
    s = slot_from_out(c, x, add);
    s[0] = (s[0] & ~(0x1fu << 27)) | top << 27;
    for (unsigned k = 0; k < XHC_HIDS; k++) {
        struct hid *h = &x->hid[k];
        uint32_t *e;

        if (!h->kind)
            continue;
        e = ctx_in(c, x, 1u + h->dci);
        ring_new(&h->ring, (struct trb *)(x->page + 1024 + 512 * k), HID_TRBS);
        h->buf = x->page + 3072 + 512 * k;
        e[0] = (uint32_t)h->interval << 16;
        e[1] = 3u << 1 | (uint32_t)EP_INT_IN << 3 | (uint32_t)h->mps << 16;
        e[2] = (uint32_t)(uint64_t)h->ring.t | 1;
        e[3] = (uint32_t)((uint64_t)h->ring.t >> 32);
        e[4] = h->mps | (uint32_t)h->mps << 16;
    }
    if (hc_command(d, c, (uint64_t)x->in, 0, trb_type(T_CONFIGURE) | (uint32_t)x->slot << 24,
                   nullptr) != CC_SUCCESS)
        return false;
    for (unsigned k = 0; k < XHC_HIDS; k++) {
        struct hid *h = &x->hid[k];

        if (!h->kind)
            continue;
        /* Some refuse these and send boot reports anyway. */
        hc_control(d, c, x, 0x21, SET_PROTOCOL, 0, h->iface, 0, nullptr);
        if (h->kind == INPUT_KEYBOARD)
            hc_control(d, c, x, 0x21, SET_IDLE, 0, h->iface, 0, nullptr);
        hid_queue(c, x, h);
        input_attach(c, x, h);
        say_dev(c, x, h->kind == INPUT_KEYBOARD ? "a keyboard" : "a mouse");
    }
    return true;
}

/* A hub (USB 2.0 §11.23.2.1): the slot made a hub's, and its ports powered. */
static bool hub_start(struct drv *d, struct xhc *c, struct xdev *x)
{
    uint8_t hd[16];
    uint32_t *s;
    unsigned wait;

    if (hc_control(d, c, x, 0xa0, GET_DESCRIPTOR, 0x2900, 0, 9, hd) != 0 || hd[1] != 0x29)
        return false;
    x->hub = true;
    x->nports = hd[2] < XHC_HUB_PORTS ? hd[2] : XHC_HUB_PORTS;
    s = slot_from_out(c, x, 1);
    s[0] |= 1u << 26;               /* a hub, its TT single: MTT clear */
    s[1] = (s[1] & 0x00ffffffu) | (uint32_t)x->nports << 24;
    if (x->speed == SPEED_HIGH)
        s[2] = (s[2] & ~(3u << 16)) | (uint32_t)(hd[3] >> 5 & 3) << 16;
    if (hc_command(d, c, (uint64_t)x->in, 0, trb_type(T_CONFIGURE) | (uint32_t)x->slot << 24,
                   nullptr) != CC_SUCCESS)
        return false;
    for (unsigned p = 1; p <= x->nports; p++)
        hc_control(d, c, x, 0x23, SET_FEATURE, F_PORT_POWER, (uint16_t)p, 0, nullptr);
    wait = 2u * hd[5];
    nap(wait > 100 ? wait : 100);
    x->hub_poll = CurrentTime();
    say_dev(c, x, "a hub");
    return true;
}

/* --- Finding one. --- */

/* The device just reset on 'port' (of hub 'parent', or the root's):
 * addressed and set up. */
static void enumerate(struct drv *d, struct xhc *c, unsigned parent, unsigned port,
                      unsigned root_port, uint8_t speed)
{
    struct xdev *p = parent == 0xff ? nullptr : &c->dev[parent], *x = nullptr;
    uint8_t desc[18], cfg[CFG_MAX];
    uint32_t slot = 0, *s, *e, total;
    unsigned i = 0, nhid = 0;
    int r;

    while (i < XHC_DEVICES && c->dev[i].used)
        i++;
    if (i == XHC_DEVICES) {
        xlog(c, "too many devices: one left alone");
        return;
    }
    x = &c->dev[i];
    *x = (struct xdev){ .used = true, .speed = speed, .port = (uint8_t)port,
                        .root_port = (uint8_t)root_port, .parent = (uint8_t)parent,
                        .page = c->pool + (uint64_t)i * 3 * PAGE };
    x->in = x->page + PAGE;
    x->out = x->page + 2 * PAGE;
    memset(x->page, 0, 3 * PAGE);
    if (p) {
        x->route = p->route | (port > 15 ? 15u : port) << (4 * p->depth);
        x->depth = (uint8_t)(p->depth + 1);
        if (p->speed == SPEED_HIGH && (speed == SPEED_FULL || speed == SPEED_LOW)) {
            x->tt_slot = p->slot;
            x->tt_port = (uint8_t)port;
        } else {
            x->tt_slot = p->tt_slot;
            x->tt_port = p->tt_port;
        }
    }
    x->mps0 = speed == SPEED_HIGH ? 64 : speed >= SPEED_SUPER ? 512 : 8;

    /* A slot, and its address (§4.3.2, §4.3.3). */
    if (hc_command(d, c, 0, 0, trb_type(T_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot ||
        slot > c->max_slots) {
        say_dev(c, x, "no slot for it");
        x->used = false;
        return;
    }
    x->slot = (uint8_t)slot;
    c->dcbaa[slot] = (uint64_t)x->out;
    ring_new(&x->ep0, (struct trb *)x->page, EP0_TRBS);
    ctx_in(c, x, 0)[1] = 3;
    s = ctx_in(c, x, 1);
    s[0] = x->route | (uint32_t)speed << 20 | 1u << 27;
    s[1] = root_port << 16;
    s[2] = x->tt_slot | (uint32_t)x->tt_port << 8;
    e = ctx_in(c, x, 2);
    e[1] = 3u << 1 | (uint32_t)EP_CONTROL << 3 | (uint32_t)x->mps0 << 16;
    e[2] = (uint32_t)(uint64_t)x->ep0.t | 1;
    e[3] = (uint32_t)((uint64_t)x->ep0.t >> 32);
    e[4] = 8;
    if ((r = hc_command(d, c, (uint64_t)x->in, 0, trb_type(T_ADDRESS) | slot << 24, nullptr)) !=
        CC_SUCCESS) {
        struct xline l = { 0 };

        path(c, x, &l);
        lput(&l, ": it wouldn't take an address: completion ");
        lnum(&l, (uint64_t)(int64_t)r, 10);
        xsay(c, &l);
        gone(d, c, i);
        return;
    }
    nap(2);

    /* Who it is: endpoint 0's packet size first, at full speed. */
    if (hc_control(d, c, x, 0x80, GET_DESCRIPTOR, 0x0100, 0, 8, desc) != 0) {
        say_dev(c, x, "no device descriptor");
        gone(d, c, i);
        return;
    }
    if (speed < SPEED_SUPER && desc[7] != x->mps0 && (desc[7] == 8 || desc[7] == 16 ||
                                                       desc[7] == 32 || desc[7] == 64)) {
        x->mps0 = desc[7];
        memset(x->in, 0, PAGE);
        ctx_in(c, x, 0)[1] = 2;
        e = ctx_in(c, x, 2);
        e[1] = 3u << 1 | (uint32_t)EP_CONTROL << 3 | (uint32_t)x->mps0 << 16;
        hc_command(d, c, (uint64_t)x->in, 0, trb_type(T_EVALUATE) | slot << 24, nullptr);
    }
    if (hc_control(d, c, x, 0x80, GET_DESCRIPTOR, 0x0100, 0, 18, desc) != 0 ||
        hc_control(d, c, x, 0x80, GET_DESCRIPTOR, 0x0200, 0, 9, cfg) != 0 || cfg[1] != 2) {
        say_dev(c, x, "no configuration descriptor");
        gone(d, c, i);
        return;
    }
    total = cfg[2] | (uint32_t)cfg[3] << 8;
    if (total > CFG_MAX)
        total = CFG_MAX;
    if (total < 9 ||
        hc_control(d, c, x, 0x80, GET_DESCRIPTOR, 0x0200, 0, (uint16_t)total, cfg) != 0) {
        say_dev(c, x, "its configuration couldn't be read");
        gone(d, c, i);
        return;
    }
    {
        struct xline l = { 0 };

        path(c, x, &l);
        lput(&l, ": ");
        lnum(&l, desc[8] | (uint32_t)desc[9] << 8, 16);
        lput(&l, ":");
        lnum(&l, desc[10] | (uint32_t)desc[11] << 8, 16);
        lput(&l, speed == SPEED_LOW ? ", low speed" : speed == SPEED_FULL ? ", full speed" :
                 speed == SPEED_HIGH ? ", high speed" : ", SuperSpeed");
        if (x->tt_slot) {
            lput(&l, ", through slot ");
            lnum(&l, x->tt_slot, 10);
            lput(&l, "'s TT");
        }
        xsay(c, &l);
    }

    /* Its interfaces: boot keyboards and mice, each with an interrupt IN
     * endpoint after it. */
    for (uint32_t at = 0, cur = XHC_HIDS; at + 2 <= total && cfg[at] >= 2 && at + cfg[at] <= total;
         at += cfg[at]) {
        const uint8_t *k = cfg + at;

        if (k[1] == 4 && k[0] >= 9) {
            cur = XHC_HIDS;
            if (k[5] == 3 && k[6] == 1 && (k[7] == 1 || k[7] == 2) && nhid < XHC_HIDS) {
                cur = nhid++;
                x->hid[cur] = (struct hid){ .iface = k[2] };
                x->hid[cur].kind = k[7] == 1 ? INPUT_KEYBOARD : INPUT_MOUSE;
            }
        } else if (k[1] == 5 && k[0] >= 7 && cur < XHC_HIDS && !x->hid[cur].dci &&
                   k[2] & 0x80 && (k[3] & 3) == 3) {
            struct hid *h = &x->hid[cur];
            uint16_t mps = (k[4] | (uint16_t)k[5] << 8) & 0x7ff;

            h->dci = (uint8_t)(2 * (k[2] & 0xf) + 1);
            h->mps = mps > 64 ? 64 : mps;
            h->interval = interval(speed, k[6]);
        }
    }
    for (unsigned k = 0; k < XHC_HIDS; k++)
        if (x->hid[k].kind && !x->hid[k].dci)
            x->hid[k].kind = 0;     /* no endpoint for it */

    if (hc_control(d, c, x, 0x00, SET_CONFIGURATION, cfg[5], 0, 0, nullptr) != 0) {
        say_dev(c, x, "it wouldn't be configured");
        gone(d, c, i);
        return;
    }
    if (desc[4] == 9) {
        if (!hub_start(d, c, x)) {
            say_dev(c, x, "a hub that couldn't be set up");
            gone(d, c, i);
        }
        return;
    }
    if (x->hid[0].kind || x->hid[1].kind) {
        if (!hid_start(d, c, x)) {
            say_dev(c, x, "its endpoints couldn't be configured");
            gone(d, c, i);
        }
        return;
    }
    say_dev(c, x, "nothing this driver reads");
}

/* --- Ports. --- */

void usb_port(struct drv *d, struct xhc *c, unsigned port)
{
    uint32_t off = PORTSC + 0x10 * (port - 1), ps = xr(c->op, off);
    int k = child(c, 0xff, port);

    xw(c->op, off, (ps & PS_KEEP) | (ps & PS_CHANGES));
    if (c->phy2 && ps & PS_CSC)     /* the K1's PHY, told (the vendor's xhci-hub.c) */
        xw(c->phy2, 0x40, xr(c->phy2, 0x40) | 1);
    if (k >= 0 && (!(ps & PS_CCS) || ps & PS_CSC))
        gone(d, c, (unsigned)k);
    else if (k >= 0)
        return;
    if (!(ps & PS_CCS) || port < c->usb2_lo || port > c->usb2_hi)
        return;
    nap(100);                       /* the connection settles (USB 2.0 §7.1.7.3) */
    ps = xr(c->op, off);
    if (!(ps & PS_CCS))
        return;
    xw(c->op, off, (ps & PS_KEEP) | PS_PR);
    for (uint64_t until = CurrentTime() + 500 * MS; !(xr(c->op, off) & PS_PRC);) {
        if (CurrentTime() > until) {
            xlog(c, "a root port wouldn't reset");
            return;
        }
        nap(5);
    }
    ps = xr(c->op, off);
    xw(c->op, off, (ps & PS_KEEP) | (ps & PS_CHANGES));
    c->pending &= ~(1u << (port - 1));      /* the reset's own change */
    if (!(ps & PS_PED))
        return;
    nap(10);
    enumerate(d, c, 0xff, port, port, (uint8_t)ps_speed(ps));
}

static bool hub_port(struct drv *d, struct xhc *c, struct xdev *x, unsigned p, uint16_t *st,
                     uint16_t *ch)
{
    uint8_t b[4];

    if (hc_control(d, c, x, 0xa3, GET_STATUS, 0, (uint16_t)p, 4, b) != 0)
        return false;
    *st = b[0] | (uint16_t)b[1] << 8;
    *ch = b[2] | (uint16_t)b[3] << 8;
    return true;
}

/* Hub x's port p, which changed or has something new on it. */
static void hub_look(struct drv *d, struct xhc *c, unsigned i, unsigned p)
{
    struct xdev *x = &c->dev[i];
    uint16_t st, ch;
    int k;

    if (!hub_port(d, c, x, p, &st, &ch))
        return;
    for (unsigned f = 0; f < 5; f++)
        if (ch & 1u << f)
            hc_control(d, c, x, 0x23, CLEAR_FEATURE, (uint16_t)(F_C_CONNECTION + f), (uint16_t)p, 0,
                       nullptr);
    k = child(c, i, p);
    if (k >= 0 && (!(st & 1) || ch & 1)) {
        gone(d, c, (unsigned)k);
        x = &c->dev[i];
        k = -1;
    }
    if (!(st & 1)) {
        x->hub_ports &= ~(1u << p);
        return;
    }
    if (k >= 0 || x->hub_ports & 1u << p)
        return;                     /* there, or tried */
    x->hub_ports |= 1u << p;
    nap(100);
    hc_control(d, c, x, 0x23, SET_FEATURE, F_PORT_RESET, (uint16_t)p, 0, nullptr);
    for (uint64_t until = CurrentTime() + 500 * MS;;) {
        nap(10);
        if (!hub_port(d, c, x, p, &st, &ch))
            return;
        if (ch & 1u << 4)
            break;
        if (CurrentTime() > until) {
            say_dev(c, x, "a port of it wouldn't reset");
            return;
        }
    }
    hc_control(d, c, x, 0x23, CLEAR_FEATURE, F_C_RESET, (uint16_t)p, 0, nullptr);
    if (!(st & 2))
        return;                     /* not enabled */
    nap(10);
    enumerate(d, c, i, p, x->root_port,
              st & 1u << 9 ? SPEED_LOW : st & 1u << 10 ? SPEED_HIGH : SPEED_FULL);
}

void usb_hubs(struct drv *d, struct xhc *c)
{
    uint64_t now = CurrentTime();

    for (unsigned i = 0; i < XHC_DEVICES; i++) {
        struct xdev *x = &c->dev[i];

        if (!x->used)
            continue;
        for (unsigned k = 0; k < XHC_HIDS; k++)
            if (x->hid[k].kind && x->hid[k].halted) {
                x->hid[k].halted = false;
                hc_recover(d, c, x, x->hid[k].dci, &x->hid[k].ring);
                hid_queue(c, x, &x->hid[k]);
            }
        if (!x->hub || x->hub_poll > now)
            continue;
        x->hub_poll = now + HUB_EVERY;
        for (unsigned p = 1; p <= x->nports && c->dev[i].used; p++)
            hub_look(d, c, i, p);
    }
}

uint64_t usb_next_poll(struct xhc *c)
{
    uint64_t next = 0;

    for (unsigned i = 0; i < XHC_DEVICES; i++) {
        const struct xdev *x = &c->dev[i];

        if (!x->used)
            continue;
        for (unsigned k = 0; k < XHC_HIDS; k++)
            if (x->hid[k].kind && x->hid[k].halted)
                return CurrentTime();
        if (x->hub && (!next || x->hub_poll < next))
            next = x->hub_poll;
    }
    return next;
}

/* A transfer's end on an endpoint other than 0. */
void usb_transfer(struct drv *, struct xhc *c, uint32_t slot, uint32_t dci, uint32_t code,
                  uint32_t residual, uint64_t trb)
{
    for (unsigned i = 0; i < XHC_DEVICES; i++) {
        struct xdev *x = &c->dev[i];

        if (!x->used || x->slot != slot)
            continue;
        for (unsigned k = 0; k < XHC_HIDS; k++) {
            struct hid *h = &x->hid[k];

            if (!h->kind || h->dci != dci)
                continue;
            if (code == CC_SUCCESS || code == CC_SHORT) {
                const struct trb *t = (const struct trb *)trb;
                const uint8_t *buf;

                if (trb < (uint64_t)h->ring.t || trb >= (uint64_t)(h->ring.t + h->ring.n - 1) ||
                    (trb - (uint64_t)h->ring.t) % sizeof *t)
                    return;         /* not one of its TRBs */
                buf = (const uint8_t *)t->ptr;
                if (residual < h->mps)
                    input_report(c, x, h, buf, h->mps - residual);
                ring_put(&h->ring, (uint64_t)buf, h->mps, trb_type(T_NORMAL) | TRB_IOC | TRB_ISP);
                hc_ring_ep(c, x->slot, h->dci);
            } else {
                h->halted = true;   /* reset from usb_hubs: not while reading events */
            }
            return;
        }
    }
}

void usb_attach_all(struct xhc *c)
{
    for (unsigned i = 0; i < XHC_DEVICES; i++)
        for (unsigned k = 0; c->dev[i].used && k < XHC_HIDS; k++)
            if (c->dev[i].hid[k].kind && c->dev[i].hid[k].dci && c->dev[i].hid[k].buf)
                input_attach(c, &c->dev[i], &c->dev[i].hid[k]);
}
