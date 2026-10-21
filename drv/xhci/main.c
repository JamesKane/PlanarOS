/*
 * drv/xhci/main.c -- USB host controllers (xHCI 1.2), and the keyboards
 * and mice behind them, for input (v5-plan step 4)
 *
 * One process drives every controller hwdb gives it: QEMU's qemu-xhci,
 * found by pci by its class (0c0330); the K1's DWC3, put in host mode
 * first (dwc3.c); the CIX's ten Cadence controllers, which UEFI left in
 * host mode (hwdb's quirks give it their xHCI windows alone).
 *
 * Bind resets and starts each controller (hc.c). Its ports are looked
 * at after, from the loop: each device found is addressed, hubs walked
 * and boot keyboards and mice read (usb.c). Their reports go to input
 * as they come, on a connection made when "input" is there and again
 * after input goes; every keyboard and mouse is attached to it again
 * then. Reports while input is away are lost.
 *
 * Interrupts come on the controller's line, or MSI-X vector 0 on PCI.
 * Where there's neither (QEMU's RISC-V PCI), the loop polls every 4 ms.
 *
 * Nothing's published: input is the class's server. A dead xhci's
 * controllers are halted by hwdb before its DMA Areas go (server.c).
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/syscalls.h>

#include "xhci.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_INPUT = 25;
constexpr unsigned MSGS = 32;
constexpr uint64_t RETRY = 500 * MS;
constexpr uint64_t POLL = 4 * MS;

bool dwc3_host(struct drv *d, struct xhc *c);

static struct xhc ctrls[DRV_DEVICES];
static unsigned nctrls;
static struct drv *the;

static struct {
    bool up;
    struct MsgPort mp;
    struct InputMsg *m[MSGS];
    bool busy[MSGS];
    uint64_t retry;                 /* when to connect again */
    unsigned lost;
} inp;

/* --- Lines for the log. --- */

void lput(struct xline *l, const char *s)
{
    while (*s && l->n < sizeof l->t - 1)
        l->t[l->n++] = *s++;
    l->t[l->n] = '\0';
}

void lnum(struct xline *l, uint64_t v, unsigned base)
{
    char dg[24];
    unsigned k = 0;

    if (base == 16)
        lput(l, "0x");
    do
        dg[k++] = "0123456789abcdef"[v % base];
    while ((v /= base) && k < sizeof dg);
    while (k && l->n < sizeof l->t - 1)
        l->t[l->n++] = dg[--k];
    l->t[l->n] = '\0';
}

void xsay(struct xhc *c, struct xline *l)
{
    drv_log(the, c->drv, l->t);
}

void xlog(struct xhc *c, const char *text)
{
    drv_log(the, c->drv, text);
}

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* --- input. --- */

static void collect(void)
{
    struct Message *r;

    while ((r = GetMsg(&inp.mp)))
        for (unsigned k = 0; k < MSGS; k++)
            if (&inp.m[k]->im_Msg == r)
                inp.busy[k] = false;
}

static void input_down(void)
{
    ClosePort(&inp.mp);
    inp.up = false;
    inp.retry = CurrentTime() + RETRY;
    for (unsigned i = 0; i < nctrls; i++)
        for (unsigned k = 0; k < XHC_DEVICES; k++)
            for (unsigned h = 0; h < XHC_HIDS; h++)
                ctrls[i].dev[k].hid[h].attached = false;
}

static void input_up(void)
{
    if (inp.up || CurrentTime() < inp.retry)
        return;
    if (ConnectPort(&inp.mp, "input", SIG_INPUT, MSGS, MSGS * sizeof *inp.m[0] + 4096) != 0) {
        inp.retry = CurrentTime() + RETRY;
        return;
    }
    for (unsigned k = 0; k < MSGS; k++) {
        inp.m[k] = AllocMsg(&inp.mp, sizeof *inp.m[k]);
        inp.busy[k] = false;
    }
    inp.up = true;
    for (unsigned i = 0; i < nctrls; i++)
        usb_attach_all(&ctrls[i]);
}

static void send(uint32_t op, struct xhc *c, struct xdev *x, struct hid *h, const uint8_t *data,
                 uint32_t len)
{
    struct InputMsg *m = nullptr;

    if (!inp.up)
        return;
    if (PortGone(&inp.mp)) {
        input_down();
        return;
    }
    collect();
    for (unsigned k = 0; k < MSGS && !m; k++)
        if (inp.m[k] && !inp.busy[k]) {
            m = inp.m[k];
            inp.busy[k] = true;
        }
    if (!m) {
        inp.lost++;                 /* input is behind: the report is lost */
        return;
    }
    memset(m, 0, sizeof *m);
    m->im_Msg.mn_Length = sizeof *m;
    m->im_Op = op;
    m->im_Device = c->index << 8 | (uint32_t)x->slot << 1 | (uint32_t)(h - x->hid);
    m->im_Kind = h->kind;
    m->im_Len = len < INPUT_REPORT_MAX ? len : INPUT_REPORT_MAX;
    if (op == INPUT_REPORT)
        memcpy(m->im_Data, data, m->im_Len);
    if (PutMsg(&inp.mp, &m->im_Msg) != 0)
        input_down();
}

void input_attach(struct xhc *c, struct xdev *x, struct hid *h)
{
    h->attached = inp.up;
    send(INPUT_ATTACH, c, x, h, nullptr, 0);
}

void input_detach(struct xhc *c, struct xdev *x, struct hid *h)
{
    if (h->attached)
        send(INPUT_DETACH, c, x, h, nullptr, 0);
    h->attached = false;
}

void input_report(struct xhc *c, struct xdev *x, struct hid *h, const uint8_t *data, uint32_t len)
{
    if (h->attached)
        send(INPUT_REPORT, c, x, h, data, len);
}

/* --- The loop. --- */

static void work(struct drv *d, struct xhc *c)
{
    hc_events(d, c);
    while (c->pending) {
        unsigned p = (unsigned)__builtin_ctz(c->pending) + 1;

        c->pending &= ~(1u << (p - 1));
        usb_port(d, c, p);
        hc_events(d, c);
    }
    usb_hubs(d, c);
}

static void interrupt(struct drv *d, struct drv_dev *dev)
{
    struct xhc *c = &ctrls[dev - d->dev];

    if (!c->cap)
        return;
    xw(c->op, USBSTS, STS_EINT);
    xw(c->rt, IMAN, IMAN_IP | IMAN_IE);
    hc_events(d, c);
}

static bool poll(struct drv *d)
{
    input_up();
    if (inp.up && PortGone(&inp.mp))
        input_down();
    for (unsigned i = 0; i < nctrls; i++)
        if (ctrls[i].cap)
            work(d, &ctrls[i]);
    return false;
}

static uint64_t deadline(struct drv *)
{
    uint64_t next = inp.up ? 0 : inp.retry;

    for (unsigned i = 0; i < nctrls; i++) {
        uint64_t t;

        if (!ctrls[i].cap)
            continue;
        t = ctrls[i].irq ? usb_next_poll(&ctrls[i]) : CurrentTime() + POLL;
        if (t && (!next || t < next))
            next = t;
    }
    return next;
}

/* --- Bind. --- */

/* MSI-X vector 0, to event 0 at the doorbell, unmasked. */
static void msix(const struct DrvDevice *info)
{
    volatile uint32_t *t = (volatile uint32_t *)info->dd_MsixTable;

    t[0] = (uint32_t)info->dd_MsiDoorbell;
    t[1] = (uint32_t)(info->dd_MsiDoorbell >> 32);
    t[2] = 0;
    t[3] = 0;
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    struct xhc *c = &ctrls[dev - d->dev];
    const struct DrvDevice *info = dev->info;
    struct xline l = { 0 };
    int r;

    the = d;
    *c = (struct xhc){ .drv = dev, .index = (unsigned)(dev - d->dev) };
    if (dev - d->dev >= nctrls)
        nctrls = (unsigned)(dev - d->dev) + 1;
    if (info->dd_Size < 0x1000)
        return DRV_DECLINE;
    c->cap = dev->regs;
    if (streq(info->dd_Key, "ky,x1-dwc3") && !dwc3_host(d, c)) {
        c->cap = nullptr;
        return DRV_DECLINE;
    }
    if (xr(c->cap, CAPLENGTH) == ~0u || (xr(c->cap, CAPLENGTH) & 0xff) < 0x20) {
        xlog(c, "no xHCI registers here");
        c->cap = nullptr;
        return DRV_DECLINE;
    }
    if (info->dd_MsiDoorbell && info->dd_MsixTable && info->dd_MsixTable >= info->dd_Base &&
        info->dd_MsixTable - info->dd_Base + 16 <= info->dd_Size)
        msix(info);
    c->irq = info->dd_Line != DRV_NOLINE;
    if ((r = hc_start(d, c)) != 0) {
        lput(&l, "the controller wouldn't start: ");
        lnum(&l, (uint64_t)-r, 10);
        if (c->op) {
            lput(&l, "; USBCMD ");
            lnum(&l, xr(c->op, USBCMD), 16);
            lput(&l, ", USBSTS ");
            lnum(&l, xr(c->op, USBSTS), 16);
        }
        xsay(c, &l);
        c->cap = nullptr;
        return DRV_DECLINE;
    }
    lput(&l, "xHCI ");            /* HCIVERSION, in BCD */
    lnum(&l, xr(c->cap, CAPLENGTH) >> 24 & 0xff, 10);
    lput(&l, ".");
    lnum(&l, xr(c->cap, CAPLENGTH) >> 20 & 0xf, 10);
    lnum(&l, xr(c->cap, CAPLENGTH) >> 16 & 0xf, 10);
    lput(&l, ", ");
    lnum(&l, c->max_ports, 10);
    lput(&l, " ports (USB 2 ");
    lnum(&l, c->usb2_lo, 10);
    lput(&l, "-");
    lnum(&l, c->usb2_hi, 10);
    lput(&l, "), ");
    lnum(&l, c->ctx, 10);
    lput(&l, "-byte contexts, ");
    lput(&l, info->dd_MsiDoorbell ? "MSI-X" : c->irq ? "its line" : "polled");
    xsay(c, &l);
    return DRV_OK;
}

static const struct drv_ops ops = {
    .name = "xhci", .bind = bind, .interrupt = interrupt, .poll = poll, .deadline = deadline,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
