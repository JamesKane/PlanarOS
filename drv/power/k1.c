/*
 * drv/power/k1.c -- the K1's power domains, and the clocks and resets of
 * the devices V5 drives
 *
 * From the vendor kernel (orange-pi-6.6-ky): drivers/soc/ky/pm_domain/
 * x1-pm_domain.c, drivers/clk/ky/ccu-ky-x1.c and drivers/reset/
 * reset-ky-x1.c. U-Boot's tree has the clock controller, the reset
 * controller and the power domains each list the same eight register
 * blocks; everything here is in one of them, the APMU at 0xd4282800,
 * and hwdb leases power that page alone (servers/hwdb/quirks.c). The
 * other blocks stay as U-Boot set them: a reference into them is left
 * untouched.
 *
 * The cells are U-Boot's. Its clock numbers are Linux's plus 41; its
 * reset numbers are Linux's; its domain numbers are mainline's (Linux's
 * vendor table minus one, BUS gone), as its LCD and HDMI nodes show.
 * The GPU has no node in U-Boot's tree, so its clock's number (0xbd) is
 * by that rule, and the set "gpu" names its resources directly. The set
 * "usb" is what the DWC3's tree leaves out: the combo PHY's mode and
 * reset, which xhci votes for before starting that PHY itself, and the
 * onboard hub's power and its ports' VBUS, GPIO 123 (Linux's tree; U-Boot's
 * says 79 and leaves it low). The GPIOs' registers are leased with the
 * APMU's page (servers/hwdb/quirks.c).
 *
 * Domains: each has a control register (bit 0 auto power-on, 1 isolation
 * released, 2 and 3 the two sleep switches, 4 hardware mode) and a bit
 * in the status register at 0xf0. The display's and audio's are switched
 * by the hardware (bits 0 and 4), the rest by software, in the vendor
 * driver's steps and delays. Clocks are a gate bit; resets of APMU
 * devices are released by setting their bits.
 */
#include <planar/syscalls.h>

#include "power.h"

constexpr uint64_t APMU = 0xd4282800;
constexpr uint64_t GPIO = 0xd4019000;
enum : uint32_t { GPLR = 0x0, GPDR = 0xc, GPSR = 0x18, GPCR = 0x24, GSDR = 0x54 };
constexpr uint32_t PWR_STATUS = 0xf0;
constexpr uint32_t PD_AUTO = 1u << 0, PD_ISO = 1u << 1, PD_SLEEP1 = 1u << 2, PD_SLEEP2 = 1u << 3,
                   PD_HW = 1u << 4;
constexpr uint64_t PD_US = 60000;   /* the vendor's 10001 tries, at 5 us */

enum : uint32_t { SW, HW };

struct domain { uint32_t cell; const char *name; uint32_t ctrl, stat, mode; };
/* A gate is on with 'mask' set and 'inv' clear. A clock with no cell
 * (NOCELL) is a set's alone: no reference names it. */
struct gate { uint32_t id; const char *name; uint32_t reg, mask, inv; };
/* A clock with a rate: its mux and divider bits ('sel') to 'value', then
 * a frequency change ('fc'), which the hardware clears when it's done
 * (the vendor's ccu_mix.c). The change finishes only with the clock's
 * gate open and its device's reset ('rst', in the same register)
 * released: on the board it never cleared with the GPU held in reset.
 * So it's made by whichever of the two goes on last; the vendor's GPU
 * driver also releases the reset, opens the clock and then sets the
 * rate (ky_init.c). */
struct rate { uint32_t reg, gate, rst, sel, value, fc; };

constexpr uint32_t NOCELL = UINT32_MAX;

static const struct domain domains[] = {
    { 0, "k1.pd.vpu",   0x0a8, 1,  SW },
    { 1, "k1.pd.gpu",   0x0d0, 0,  SW },
    { 2, "k1.pd.lcd",   0x380, 12, HW },
    { 3, "k1.pd.isp",   0x37c, 2,  SW },
    { 4, "k1.pd.audio", 0x378, 11, HW },
    { 5, "k1.pd.gnss",  0x13c, 6,  SW },
    { 6, "k1.pd.hdmi",  0x3f4, 15, HW },
};

static const struct gate clocks[] = {
    { 0xa5, "k1.clk.dpu-mclk",  0x04c, 1u << 0, 0 },
    { 0xa6, "k1.clk.dpu-esc",   0x044, 1u << 2, 0 },
    { 0xa7, "k1.clk.dpu-bit",   0x044, 1u << 16, 0 },
    { 0xa8, "k1.clk.dpu-pxclk", 0x04c, 1u << 16, 0 },
    { 0xa9, "k1.clk.dpu-hclk",  0x044, 1u << 5, 0 },
    { 0xb1, "k1.clk.sdh-axi",   0x054, 1u << 3, 0 },
    { 0xb2, "k1.clk.sdh0",      0x054, 1u << 4, 0 },
    { 0xb3, "k1.clk.sdh1",      0x058, 1u << 4, 0 },
    { 0xb4, "k1.clk.sdh2",      0x0e0, 1u << 4, 0 },
    { 0xb7, "k1.clk.usb30",     0x05c, 1u << 8, 0 },
    { 0xbd, "k1.clk.gpu",       0x0cc, 1u << 4, 0 },
    { 0xc1, "k1.clk.hdmi",      0x1b8, 1u << 0, 0 },
    /* The combo PHY's port A to USB 3, not PCIe (PCIE_PHY_SEL's MODE_SEL;
     * the vendor's phy-ky-x1-combphy.c). */
    { NOCELL, "k1.sel.usb3",    0x110, 1u << 3, 0 },
};

static const struct rate rates[] = {
    /* The GPU's 614.4 MHz, the DDK's (ky/sysconfig.h): pll1_d4_614p4 (mux
     * 0, bits 20:18), divided by 1 (bits 14:12). U-Boot leaves pll2_d3 / 6. */
    { 0x0cc, 1u << 4, 1u << 1, 7u << 18 | 7u << 12, 0, 1u << 15 },
};

static const struct gate resets[] = {
    { 0x3d, "k1.rst.lcd",       0x044, 1u << 4, 0 },
    { 0x3e, "k1.rst.dsi-esc",   0x044, 1u << 3, 0 },
    { 0x40, "k1.rst.mipi",      0x044, 1u << 15, 0 },
    { 0x44, "k1.rst.lcd-mclk",  0x04c, 1u << 9, 0 },
    { 0x47, "k1.rst.sdh-axi",   0x054, 1u << 0, 0 },
    { 0x48, "k1.rst.sdh0",      0x054, 1u << 1, 0 },
    { 0x4c, "k1.rst.usb3",      0x05c, 7u << 9, 0 },
    { 0x52, "k1.rst.gpu",       0x0cc, 1u << 1, 0 },
    { 0x53, "k1.rst.sdh2",      0x0e0, 1u << 1, 0 },
    { 0x58, "k1.rst.hdmi",      0x1b8, 1u << 9, 0 },
    /* The combo PHY's: bit 8 is its global reset. */
    { 0x59, "k1.rst.pcie0",     0x3cc, 7u << 3, 1u << 8 },
};

/* Supplies that are a GPIO driven high (the vendor's gpio-x1.c): 32 to a
 * bank, banks at 0, 4, 8 and 0x100. */
struct supply { const char *name; uint32_t gpio; };

static const struct supply supplies[] = {
    { "k1.gpio.usb-vbus", 123 },
};

constexpr unsigned NDOM = sizeof domains / sizeof domains[0];
constexpr unsigned NCLK = sizeof clocks / sizeof clocks[0];
constexpr unsigned NRST = sizeof resets / sizeof resets[0];
constexpr unsigned NSUP = sizeof supplies / sizeof supplies[0];

static volatile uint8_t *apmu, *gpio;
static int first = -1;              /* pw_res index of domains[0]; clocks, then resets, follow */

static volatile uint32_t *reg(uint32_t off)
{
    return (volatile uint32_t *)(apmu + off);
}

static void rmw(uint32_t off, uint32_t clear, uint32_t set)
{
    *reg(off) = (*reg(off) & ~clear) | set;
}

static bool init(void)
{
    for (unsigned i = 0; i < pw_ndev; i++)
        if (pw_dev[i].be == &pw_k1 && pw_dev[i].regs && pw_dev[i].base <= APMU &&
            APMU + 0x400 <= pw_dev[i].base + pw_dev[i].size)
            apmu = pw_dev[i].regs + (APMU - pw_dev[i].base);
    if (!apmu) {
        pw_say("power: k1: the APMU isn't in my registers");
        return false;
    }
    first = (int)pw_nres;
    for (unsigned i = 0; i < NDOM; i++)
        if (pw_add(&pw_k1, domains[i].name, POWER_DOMAIN, i, 0, 0) < 0)
            return false;
    for (unsigned i = 0; i < NCLK; i++)
        if (pw_add(&pw_k1, clocks[i].name, POWER_CLOCK, i, 0, 0) < 0)
            return false;
    for (unsigned i = 0; i < NRST; i++)
        if (pw_add(&pw_k1, resets[i].name, POWER_RESET, i, 0, 0) < 0)
            return false;
    if ((gpio = pw_map(GPIO, 0x200)))
        for (unsigned i = 0; i < NSUP; i++)
            if (pw_add(&pw_k1, supplies[i].name, POWER_SUPPLY, i, 0, 0) < 0)
                return false;
    return true;
}

static bool is(const char *key, const char *want)
{
    while (*key && *key == *want)
        key++, want++;
    return *key == *want;
}

static int xlate(unsigned dev, const struct PowerRef *r)
{
    const char *key = pw_dev[dev].key;

    if (r->pr_NCells != 1)
        return -1;
    if (r->pr_Kind == POWER_DOMAIN && is(key, "ky,x1-pm-domain")) {
        for (unsigned i = 0; i < NDOM; i++)
            if (domains[i].cell == r->pr_Cells[0])
                return first + (int)i;
    } else if (r->pr_Kind == POWER_CLOCK && is(key, "ky,x1-ccu")) {
        for (unsigned i = 0; i < NCLK; i++)
            if (clocks[i].id == r->pr_Cells[0])
                return first + (int)(NDOM + i);
    } else if (r->pr_Kind == POWER_RESET && is(key, "ky,x1-reset")) {
        for (unsigned i = 0; i < NRST; i++)
            if (resets[i].id == r->pr_Cells[0])
                return first + (int)(NDOM + NCLK + i);
    }
    return -1;
}

static void say_reg(const char *what, uint32_t off)
{
    char line[96];
    unsigned n = 0;
    uint32_t v = *reg(off);

    while (*what && n < 70)
        line[n++] = *what++;
    for (int sh = 28; sh >= 0; sh -= 4)
        line[n++] = "0123456789abcdef"[v >> sh & 0xf];
    line[n] = '\0';
    pw_say(line);
}

/* The rates in register 'off', set once their gate and reset are both on. */
static int64_t rate(uint32_t off)
{
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        const struct rate *t = &rates[i];
        uint32_t v = *reg(off);

        if (t->reg != off || (v & (t->gate | t->rst)) != (t->gate | t->rst) ||
            (v & t->sel) == t->value)
            continue;
        rmw(off, t->sel, t->value | t->fc);
        if (!pw_poll32(reg(off), t->fc, 0, 1000)) {
            say_reg("power: k1: a frequency change didn't finish: ", off);
            return ERR_TIMEOUT;
        }
    }
    return 0;
}

static int64_t domain_set(const struct domain *d, bool on)
{
    uint32_t bit = 1u << d->stat;

    if (d->mode == HW) {
        rmw(d->ctrl, on ? 0 : PD_AUTO | PD_HW, on ? PD_AUTO | PD_HW : 0);
        if (on)
            pw_delay_us(300);
    } else if (on) {
        rmw(d->ctrl, 0, PD_SLEEP1);
        pw_delay_us(25);
        rmw(d->ctrl, 0, PD_SLEEP1 | PD_SLEEP2);
        pw_delay_us(25);
        rmw(d->ctrl, 0, PD_ISO);
        pw_delay_us(15);
    } else {
        rmw(d->ctrl, PD_ISO, 0);
        pw_delay_us(15);
        rmw(d->ctrl, PD_SLEEP1 | PD_SLEEP2, 0);
        pw_delay_us(15);
    }
    if (pw_poll32(reg(PWR_STATUS), bit, on ? bit : 0, PD_US))
        return 0;
    say_reg("power: k1: a domain's status didn't follow: ", PWR_STATUS);
    return ERR_TIMEOUT;
}

static volatile uint32_t *bank(uint32_t n, uint32_t off)
{
    static const uint32_t at[] = { 0x0, 0x4, 0x8, 0x100 };

    return (volatile uint32_t *)(gpio + at[n / 32 % 4] + off);
}

static int64_t set(struct pw_res *r, bool on)
{
    const struct gate *g;

    if (r->kind == POWER_SUPPLY) {
        uint32_t n = supplies[r->a].gpio;

        *bank(n, on ? GPSR : GPCR) = 1u << n % 32;
        *bank(n, GSDR) = 1u << n % 32;      /* an output */
        if (on)
            pw_delay_us(10000);             /* the vendor's vbus_delay_ms */
        return 0;
    }
    if (r->kind == POWER_DOMAIN)
        return domain_set(&domains[r->a], on);
    g = r->kind == POWER_CLOCK ? &clocks[r->a] : &resets[r->a];
    rmw(g->reg, on ? g->inv : g->mask, on ? g->mask : g->inv);
    return on ? rate(g->reg) : 0;
}

static int64_t get(struct pw_res *r)
{
    const struct gate *g;

    if (r->kind == POWER_SUPPLY) {
        uint32_t n = supplies[r->a].gpio;

        return (*bank(n, GPDR) & *bank(n, GPLR)) >> n % 32 & 1;
    }

    if (r->kind == POWER_DOMAIN)
        return *reg(PWR_STATUS) >> domains[r->a].stat & 1;
    g = r->kind == POWER_CLOCK ? &clocks[r->a] : &resets[r->a];
    return (*reg(g->reg) & (g->mask | g->inv)) == g->mask;
}

static const char *const keys[] = { "ky,x1-pm-domain", "ky,x1-ccu", "ky,x1-reset", nullptr };
static const char *const gpu[] = { "gpu", "k1.pd.gpu", "k1.clk.gpu", "k1.rst.gpu", nullptr };
/* For xhci's DWC3: the USB 3 PHY's port mode and reset, the hub's VBUS. */
static const char *const usb[] = {
    "usb", "k1.sel.usb3", "k1.rst.pcie0", "k1.gpio.usb-vbus", nullptr,
};
static const char *const *const sets[] = { gpu, usb, nullptr };

const struct pw_backend pw_k1 = {
    .name = "k1", .keys = keys, .init = init, .xlate = xlate, .set = set, .get = get,
    .sets = sets,
};
