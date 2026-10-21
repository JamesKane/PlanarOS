/*
 * drv/xhci/dwc3.c -- the K1's DWC3, made a host (the vendor kernel's
 * drivers/usb/dwc3/core.c, dwc3_core_init)
 *
 * Its xHCI registers are the first 32 KiB of its window, its own from
 * 0xc100. power has its clock on and its reset released (hwdb's vote,
 * for the glue node's references). First its PHYs: the combo PHY's port
 * A as USB 3's, once power has its mode and reset and the hub's VBUS on
 * (xhci's own vote, the set "usb"), since the core needs its pipe clock
 * even with only USB 2's ports walked (usb.c); and the USB 2 PHY (the vendor's
 * phy-x1-ci-usb2.c), its PLL locked, then its tuning. Then the core: the
 * vendor's tree sets five quirks (x1_orangepi-rv2.dts), which come to
 * the PHYs never suspended and UTMI at 8 bits; a soft reset; and the
 * port's direction set to host.
 */
#include <stddef.h>

#include <planar/devices/power.h>
#include <planar/syscalls.h>

#include "xhci.h"

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_POWER = 26;
constexpr uint64_t PHY2 = 0xc0a30000, PHY3 = 0xc0b10000;

enum : uint32_t { PHY_PLL = 0x04, PHY_0D = 0x34, PHY_HSDAC = 0xa4, PHY_04 = 0x10 };
enum : uint32_t { P3_08 = 0x08, P3_18 = 0x18, P3_48 = 0x48, P3_58 = 0x58, P3_68 = 0x68 };

/* power's connection: its vote for "usb" stands while it's open. */
static struct MsgPort power;

static bool leased(struct drv *d, uint64_t at, uint64_t size)
{
    for (unsigned k = 0; k < d->info.hm_NRanges; k++) {
        const struct DrvRange *rg = &d->info.hm_Range[k];

        if (at >= rg->dr_Base && at + size <= rg->dr_Base + rg->dr_Size)
            return true;
    }
    return false;
}

static bool vote(struct xhc *c)
{
    struct PowerMsg *m;
    const char *name = "usb";

    if (ConnectPort(&power, "power", SIG_POWER, 2, 2 * sizeof *m) != 0 ||
        !(m = AllocMsg(&power, sizeof *m))) {
        xlog(c, "no power to vote for USB");
        return false;
    }
    memset(m, 0, sizeof *m);
    m->pm_Msg.mn_Length = sizeof *m;
    m->pm_Op = POWER_ON;
    m->pm_Tag = 1;
    for (unsigned i = 0; name[i]; i++)
        m->pm_Name[i] = name[i];
    if (DoMsg(&power, &m->pm_Msg) != 0 || m->pm_Msg.mn_Error != 0) {
        xlog(c, "power refused USB's set");
        return false;
    }
    return true;
}

/* The combo PHY's port A as USB 3's, its PLL locked (the vendor's
 * phy-ky-x1-combphy.c, 72-159): without its pipe clock the core never
 * finishes an xHCI reset. */
static bool phy3(struct xhc *c)
{
    volatile uint8_t *p = (volatile uint8_t *)PHY3;

    if (!vote(c))
        return false;
    xw(p, P3_68, 0);
    xw(p, P3_48, 0x603a2276);
    xw(p, P3_08, 0x97c);
    xw(p, P3_18, 0);
    for (uint64_t until = CurrentTime() + 50 * MS; !(xr(p, P3_08) & 1);) {
        if (CurrentTime() > until) {
            xlog(c, "the USB 3 PHY's PLL didn't lock");
            return false;
        }
        nap(1);
    }
    xw(p, P3_58, (xr(p, P3_58) & ~(7u << 8)) | 3u << 8);
    return true;
}

enum : uint32_t {
    GCTL = 0xc110, GSTS = 0xc118, GUCTL1 = 0xc11c, GSNPSID = 0xc120, GUSB2PHYCFG = 0xc200,
    GUSB3PIPECTL = 0xc2c0,
    DCTL = 0xc704,
};

bool dwc3_host(struct drv *d, struct xhc *c)
{
    volatile uint8_t *r = c->cap;
    uint32_t id, v;
    struct xline l = { 0 };

    if (c->drv->info->dd_Size < 0x10000)
        return false;
    /* The PHYs, if hwdb leased them too. */
    if (!leased(d, PHY2, 0x200) || !leased(d, PHY3, 0x800)) {
        xlog(c, "its PHYs aren't leased");
        return false;
    }
    if (!phy3(c))
        return false;
    c->phy2 = (volatile uint8_t *)PHY2;
    nap(1);
    for (uint64_t until = CurrentTime() + 50 * MS; !(xr(c->phy2, PHY_PLL) & 1);) {
        if (CurrentTime() > until) {
            xlog(c, "the USB 2 PHY's PLL didn't lock");
            return false;
        }
        nap(1);
    }
    xw(c->phy2, PHY_PLL, 0x60ef);
    xw(c->phy2, PHY_0D, 0x1c);
    xw(c->phy2, PHY_HSDAC, (xr(c->phy2, PHY_HSDAC) & ~0xfu) | 0xc | 1u << 4);
    xw(c->phy2, PHY_04, xr(c->phy2, PHY_04) | 1u << 2);

    id = xr(r, GSNPSID);
    if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u) {
        lput(&l, "not a DWC3: GSNPSID ");
        lnum(&l, id, 16);
        xsay(c, &l);
        return false;
    }
    /* The PHYs: never suspended, the pipe's power changes not delayed;
     * UTMI, 8 bits, turnaround 9 (core.c 634-751). */
    xw(r, GUSB3PIPECTL, xr(r, GUSB3PIPECTL) & ~(1u << 27 | 1u << 17 | 1u << 18));
    v = xr(r, GUSB2PHYCFG) & ~(1u << 4 | 1u << 3 | 0xfu << 10 | 1u << 6 | 1u << 8);
    xw(r, GUSB2PHYCFG, v | 9u << 10);

    /* The core reset (DCTL.CSFTRST), then host (GCTL.PrtCapDir = 1). */
    xw(r, DCTL, xr(r, DCTL) | 1u << 30);
    for (uint64_t until = CurrentTime() + 500 * MS; xr(r, DCTL) & 1u << 30;) {
        if (CurrentTime() > until) {
            xlog(c, "the DWC3 wouldn't reset");
            return false;
        }
        nap(1);
    }
    nap(10);
    v = xr(r, GCTL) & ~(3u << 4 | 3u << 12);
    xw(r, GCTL, v | 1u << 12);
    xw(r, GUCTL1, xr(r, GUCTL1) | 1u << 28 | 1u << 17);
    nap(10);
    lput(&l, "DWC3 ");
    lnum(&l, id, 16);
    lput(&l, " made a host: GSTS ");
    lnum(&l, xr(r, GSTS), 16);
    xsay(c, &l);
    return true;
}
