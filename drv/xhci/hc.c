/*
 * drv/xhci/hc.c -- the controller: reset and start, the command and event
 * rings, and control transfers (xHCI 1.2 §4.2, §4.6, §4.9, §4.11)
 *
 * Everything the controller reads or writes is in DMA Areas below 4 GiB,
 * not cached by the CPU: neither board's controllers snoop, and there is
 * then no cache to keep in step. A barrier orders what's written there
 * before the doorbell.
 *
 * Commands and control transfers are waited for here, the event ring
 * read until their completion comes: they're made only while a device is
 * being set up. Events for others meanwhile (a keyboard's reports) are
 * handled as they come.
 */
#include <stddef.h>

#include <planar/syscalls.h>

#include "xhci.h"

void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_NAP = 24;
constexpr uint32_t PAGE = 4096;
constexpr uint32_t CMD_TRBS = 256, EV_TRBS = 256;
constexpr uint32_t CTRL_BUF = 2048, CTRL_MAX = 1024;    /* in a device's first page */

uint32_t xr(const volatile uint8_t *base, uint32_t off)
{
    return *(const volatile uint32_t *)(base + off);
}

void xw(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

void xw64(volatile uint8_t *base, uint32_t off, uint64_t v)
{
    xw(base, off, (uint32_t)v);
    xw(base, off + 4, (uint32_t)(v >> 32));
}

void xbarrier(void)
{
    drv_io_barrier();
}

void *xdma(struct drv *d, uint64_t size)
{
    void *at;
    int64_t e = AllocDmaArea(drv_lease(d), size, AREAF_NC | AREAF_DMA32, &at);

    if (e == ERR_NOSYS)         /* a CPU that can't leave memory uncached: QEMU's, coherent */
        e = AllocDmaArea(drv_lease(d), size, AREAF_DMA32, &at);
    return e < 0 ? nullptr : at;
}

void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_NAP);
    Wait(1ULL << SIG_NAP);
}

bool ring_new(struct ring *r, struct trb *t, uint32_t n)
{
    if (!t)
        return false;
    *r = (struct ring){ .t = t, .n = n, .cycle = 1 };
    for (uint32_t i = 0; i < n; i++)
        t[i] = (struct trb){ 0 };
    t[n - 1].ptr = (uint64_t)t;
    t[n - 1].control = trb_type(T_LINK) | TRB_TC;
    return true;
}

/* A TRB at the enqueue pointer, its cycle bit written last; past the
 * link, which takes the chain bit of the TRB before it (§4.11.5.1). */
struct trb *ring_put(struct ring *r, uint64_t ptr, uint32_t status, uint32_t control)
{
    struct trb *t = &r->t[r->enq];

    t->ptr = ptr;
    t->status = status;
    xbarrier();
    t->control = control | r->cycle;
    if (++r->enq == r->n - 1) {
        struct trb *l = &r->t[r->n - 1];

        xbarrier();
        l->control = trb_type(T_LINK) | TRB_TC | (control & TRB_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return t;
}

uint32_t *ctx_in(struct xhc *c, struct xdev *x, unsigned i)
{
    return (uint32_t *)(x->in + i * c->ctx);
}

uint32_t *ctx_out(struct xhc *c, struct xdev *x, unsigned i)
{
    return (uint32_t *)(x->out + i * c->ctx);
}

/* --- Events (§4.9.4). --- */

unsigned hc_events(struct drv *d, struct xhc *c)
{
    unsigned transfers = 0;

    for (;;) {
        struct trb *e = &c->ev[c->ev_deq];
        uint32_t ctl = e->control, st, slot, dci, code;
        uint64_t ptr;

        if ((ctl & TRB_C) != c->ev_cycle)
            break;
        xbarrier();
        ptr = e->ptr;
        st = e->status;
        code = st >> 24;
        slot = ctl >> 24;
        dci = ctl >> 16 & 0x1f;
        if (++c->ev_deq == c->ev_n) {
            c->ev_deq = 0;
            c->ev_cycle ^= 1;
        }
        switch (ctl >> 10 & 0x3f) {
        case T_COMMAND_EV:
            if (c->wait_trb && ptr == c->wait_trb && !c->wait_slot) {
                c->wait_done = true;
                c->wait_code = code;
                c->got_slot = slot;
            }
            break;
        case T_TRANSFER_EV:
            if (c->wait_trb && c->wait_slot && slot == c->wait_slot && dci == 1 &&
                (ptr == c->wait_trb || (code != CC_SUCCESS && code != CC_SHORT))) {
                c->wait_done = true;
                c->wait_code = code;
            } else {
                usb_transfer(d, c, slot, dci, code, st & 0xffffff, ptr);
                transfers++;
            }
            break;
        case T_PORT_EV:
            if ((ptr >> 24 & 0xff) >= 1 && (ptr >> 24 & 0xff) <= 32)
                c->pending |= 1u << ((ptr >> 24 & 0xff) - 1);
            break;
        default:
            break;
        }
    }
    /* Always, events or not: EHB is cleared only here, and while it's
     * set the controller raises no interrupt. The loop's poll can read
     * the events before the interrupt's handler clears IP, which then
     * finds none: written only after events, EHB could stay set until
     * the next timer. */
    xw64(c->rt, ERDP, (uint64_t)&c->ev[c->ev_deq] | ERDP_EHB);
    return transfers;
}

/* Until what's waited for has come, or 'ms' have gone. */
static bool wait(struct drv *d, struct xhc *c, uint64_t ms)
{
    uint64_t start = CurrentTime(), until = start + ms * MS;

    while (!c->wait_done) {
        hc_events(d, c);
        if (c->wait_done)
            break;
        if (CurrentTime() > until || xr(c->op, USBSTS) & STS_HSE) {
            c->wait_trb = 0;
            return false;
        }
        if (CurrentTime() - start > MS / 4)
            nap(1);
    }
    c->wait_trb = 0;
    return true;
}

/* A command (§4.6): its completion code, or -1 if none came. */
int hc_command(struct drv *d, struct xhc *c, uint64_t ptr, uint32_t status, uint32_t control,
               uint32_t *slot)
{
    struct trb *t = ring_put(&c->cmd, ptr, status, control);

    c->wait_trb = (uint64_t)t;
    c->wait_slot = 0;
    c->wait_done = false;
    xbarrier();
    xw(c->db, 0, 0);
    if (!wait(d, c, 1000))
        return -1;
    if (slot)
        *slot = c->got_slot;
    return (int)c->wait_code;
}

void hc_ring_ep(struct xhc *c, uint8_t slot, uint8_t dci)
{
    xbarrier();
    xw(c->db, 4u * slot, dci);
}

/* An endpoint halted by an error, going again from where its ring is
 * (§4.6.8, §4.6.10). */
void hc_recover(struct drv *d, struct xhc *c, struct xdev *x, uint8_t dci, struct ring *r)
{
    hc_command(d, c, 0, 0, trb_type(T_RESET_EP) | (uint32_t)dci << 16 | (uint32_t)x->slot << 24,
               nullptr);
    hc_command(d, c, (uint64_t)&r->t[r->enq] | r->cycle, 0,
               trb_type(T_SET_DEQ) | (uint32_t)dci << 16 | (uint32_t)x->slot << 24, nullptr);
}

/* A control transfer on endpoint 0 (§4.11.2.2): 0, its completion code
 * if it failed, or -1 if it didn't end. 'len' bytes in or out of 'data'. */
int hc_control(struct drv *d, struct xhc *c, struct xdev *x, uint8_t type, uint8_t req,
               uint16_t value, uint16_t index, uint16_t len, void *data)
{
    bool in = type & 0x80;
    uint8_t *buf = x->page + CTRL_BUF;
    uint64_t setup = type | (uint32_t)req << 8 | (uint32_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)len << 48;
    struct trb *t;

    if (len > CTRL_MAX)
        return -1;
    if (!in && len)
        memcpy(buf, data, len);
    ring_put(&x->ep0, setup, 8, trb_type(T_SETUP) | TRB_IDT | (len ? (in ? 3u : 2u) << 16 : 0));
    if (len)
        ring_put(&x->ep0, (uint64_t)buf, len, trb_type(T_DATA) | (in ? TRB_DIR_IN : 0));
    t = ring_put(&x->ep0, 0, 0, trb_type(T_STATUS) | TRB_IOC | (len && in ? 0 : TRB_DIR_IN));
    c->wait_trb = (uint64_t)t;
    c->wait_slot = x->slot;
    c->wait_done = false;
    hc_ring_ep(c, x->slot, 1);
    if (!wait(d, c, 1000))
        return -1;
    if (c->wait_code != CC_SUCCESS && c->wait_code != CC_SHORT) {
        int code = (int)c->wait_code;

        hc_recover(d, c, x, 1, &x->ep0);
        return code;
    }
    if (in && len)
        memcpy(data, buf, len);
    return 0;
}

/* --- Starting (§4.2). --- */

/* The firmware's hold on it, let go (§4.22.1): if it doesn't, it's
 * taken anyway. Its SMIs off. */
static void handoff(struct xhc *c, uint32_t at)
{
    uint64_t until = CurrentTime() + 1000 * MS;

    if (xr(c->cap, at) & 1u << 16) {
        xw(c->cap, at, xr(c->cap, at) | 1u << 24);
        while (xr(c->cap, at) & 1u << 16 && CurrentTime() < until)
            nap(1);
        if (xr(c->cap, at) & 1u << 16)
            xlog(c, "the firmware didn't let go of it: taken");
    }
    xw(c->cap, at + 4, (xr(c->cap, at + 4) & ~0xe011u) | 0xe0000000u);
}

static bool until_clear(volatile uint8_t *base, uint32_t off, uint32_t bits, uint64_t ms)
{
    uint64_t until = CurrentTime() + ms * MS;

    while (xr(base, off) & bits) {
        if (xr(base, off) == ~0u || CurrentTime() > until)
            return false;
        nap(1);
    }
    return true;
}

int hc_start(struct drv *d, struct xhc *c)
{
    volatile uint8_t *b = c->cap;
    uint32_t hcs1 = xr(b, HCSPARAMS1), hcs2 = xr(b, HCSPARAMS2), hcc = xr(b, HCCPARAMS1);
    uint32_t sp = (hcs2 >> 27 & 0x1f) | (hcs2 >> 21 & 0x1f) << 5;
    uint32_t size = (uint32_t)c->drv->info->dd_Size;
    uint8_t *base;
    uint64_t *erst;

    c->op = b + (xr(b, CAPLENGTH) & 0xff);
    c->rt = b + (xr(b, RTSOFF) & ~0x1fu);
    c->db = b + (xr(b, DBOFF) & ~0x3u);
    c->max_slots = (hcs1 & 0xff) < XHC_DEVICES ? (hcs1 & 0xff) : XHC_DEVICES;
    c->max_ports = hcs1 >> 24;
    c->ctx = hcc & HCC_CSZ ? 64 : 32;
    c->ac64 = hcc & HCC_AC64;
    if (c->max_slots == 0 || c->max_ports == 0 || c->max_ports > 32 || sp > 256 ||
        !(xr(c->op, PAGESIZE) & 1))
        return -20;                 /* 4 KiB pages, a page of scratchpad pointers */

    /* Its extended capabilities (§7): the firmware's hold, and which
     * ports are USB 2's. Others (the CIX's own, 0xc0 and up) are skipped. */
    c->usb2_lo = 1;
    c->usb2_hi = (uint8_t)c->max_ports;
    for (uint32_t at = (hcc >> 16) * 4, n = 0; at && at + 12 <= size && n < 64; n++) {
        uint32_t v = xr(b, at);

        if ((v & 0xff) == 1)
            handoff(c, at);
        if ((v & 0xff) == 2 && v >> 24 == 2) {
            uint32_t p = xr(b, at + 8);

            c->usb2_lo = (uint8_t)(p & 0xff);
            c->usb2_hi = (uint8_t)((p & 0xff) + (p >> 8 & 0xff) - 1);
        }
        at = v >> 8 & 0xff ? at + (v >> 8 & 0xff) * 4 : 0;
    }

    /* Stopped, then reset: whatever the firmware or a dead driver left. */
    xw(c->op, USBCMD, xr(c->op, USBCMD) & ~(CMD_RS | CMD_INTE));
    for (uint64_t until = CurrentTime() + 100 * MS; !(xr(c->op, USBSTS) & STS_HCH);)
        if (xr(c->op, USBSTS) == ~0u || CurrentTime() > until)
            return -21;
        else
            nap(1);
    xw(c->op, USBCMD, CMD_HCRST);
    nap(1);
    if (!until_clear(c->op, USBCMD, CMD_HCRST, 1000))
        return -22;
    if (!until_clear(c->op, USBSTS, STS_CNR, 1000))
        return -26;

    /* The device context array and its scratchpads; the rings (§4.2). */
    if (!(base = xdma(d, 3 * PAGE)) || !(c->pool = xdma(d, (uint64_t)XHC_DEVICES * 3 * PAGE)))
        return -23;
    c->dcbaa = (uint64_t *)base;
    erst = (uint64_t *)(base + 0x800);
    if (sp) {
        uint64_t *array = (uint64_t *)(base + 0xc00), *bufs;

        if (sp > 128 && !(array = xdma(d, PAGE)))
            return -23;
        if (!(bufs = xdma(d, (uint64_t)sp * PAGE)))
            return -23;
        for (uint32_t i = 0; i < sp; i++)
            array[i] = (uint64_t)bufs + (uint64_t)i * PAGE;
        c->dcbaa[0] = (uint64_t)array;
    }
    if (!ring_new(&c->cmd, (struct trb *)(base + PAGE), CMD_TRBS))
        return -23;
    c->ev = (struct trb *)(base + 2 * PAGE);
    c->ev_n = EV_TRBS;
    c->ev_cycle = 1;
    erst[0] = (uint64_t)c->ev;
    erst[1] = EV_TRBS;
    xbarrier();

    xw(c->op, CONFIG, c->max_slots);
    xw64(c->op, DCBAAP, (uint64_t)c->dcbaa);
    xw64(c->op, CRCR, (uint64_t)c->cmd.t | 1);
    xw(c->rt, ERSTSZ, 1);
    xw64(c->rt, ERDP, (uint64_t)c->ev);
    xw64(c->rt, ERSTBA, (uint64_t)erst);
    /* At least 32 ticks between interrupts: 8 us in xHCI's 250 ns ticks,
     * 1 ms in the K1's DWC3's, which count at 32 kHz. 4000 (1 ms as the
     * spec has it) was 125 ms there: the mouse's reports came 4 at a
     * time, 8 times a second (v7-plan step 6). */
    xw(c->rt, IMOD, 32);
    xw(c->rt, IMAN, IMAN_IP | IMAN_IE);
    xw(c->op, USBSTS, STS_EINT | STS_PCD | STS_HSE);
    xw(c->op, USBCMD, CMD_RS | (c->irq ? CMD_INTE : 0));
    for (uint64_t until = CurrentTime() + 100 * MS; xr(c->op, USBSTS) & STS_HCH;)
        if (CurrentTime() > until)
            return -24;
        else
            nap(1);
    if (hc_command(d, c, 0, 0, trb_type(T_NOOP_CMD), nullptr) != CC_SUCCESS)
        return -25;

    /* Every port powered, and each looked at once. */
    if (hcc & HCC_PPC) {
        for (uint32_t p = 1; p <= c->max_ports; p++) {
            uint32_t ps = xr(c->op, PORTSC + 0x10 * (p - 1));

            if (!(ps & PS_PP))
                xw(c->op, PORTSC + 0x10 * (p - 1), (ps & PS_KEEP) | PS_PP);
        }
        nap(20);
    }
    c->pending = c->max_ports == 32 ? ~0u : (1u << c->max_ports) - 1;
    return 0;
}
