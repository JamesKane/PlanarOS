/*
 * drv/pci/main.c -- a PCI host bridge's ECAM (PCI Express Base 6.0 §7.2.2),
 * and the functions behind it (§7.5)
 *
 * A bus driver and an hwdb source (§11.6, servers/hwdb/pci.c). Bind
 * asks hwdb for the bridge's buses and windows, walks config space from
 * the first bus down through each bridge the firmware set up, and sends
 * each function it finds to hwdb, which adds and binds it.
 *
 * The firmware trains the links and numbers the buses on both boards,
 * so this only reads what's there, with one exception: a memory BAR
 * left at 0 or outside every window (QEMU booted without firmware)
 * gets an address from a window here, aligned to its size and clear of
 * the BARs that have one. Then memory decode and bus mastering are
 * turned on, and MSI-X, when hwdb says the bridge's MSIs reach an ITS.
 */
#include <planar/drv.h>
#include <planar/syscalls.h>

constexpr unsigned MAX_FUNCS = 64;

enum : uint32_t {
    ID = 0x00, COMMAND = 0x04, CLASS = 0x08, HEADER = 0x0c, BAR0 = 0x10, BUSES = 0x18,
    CAPS = 0x34,
};
enum : uint32_t { CMD_MEM = 1u << 1, CMD_MASTER = 1u << 2, CMD_NOINTX = 1u << 10 };
enum : uint32_t { STATUS_CAPS = 1u << 20 };     /* in the COMMAND dword */
enum : uint8_t { CAP_PCIE = 0x10, CAP_MSIX = 0x11 };
enum : uint32_t { MSIX_ENABLE = 1u << 31, MSIX_MASKALL = 1u << 30 };   /* in its first dword */

struct func {
    unsigned bus, dev, fn;
    struct PciFunction f;
    uint32_t msix_cap;          /* its MSI-X capability's offset, or 0 */
    bool bridge;
};

static struct PciHost host;
static uint64_t ecam;
static struct func funcs[MAX_FUNCS];
static unsigned nfuncs, assigned;

static volatile uint32_t *cfg(unsigned bus, unsigned dev, unsigned fn, uint32_t off)
{
    return (volatile uint32_t *)(ecam + ((uint64_t)(bus - host.ph_FirstBus) << 20 |
                                         dev << 15 | fn << 12 | off));
}

static uint32_t rd(const struct func *f, uint32_t off)
{
    return *cfg(f->bus, f->dev, f->fn, off);
}

static void wr(const struct func *f, uint32_t off, uint32_t v)
{
    *cfg(f->bus, f->dev, f->fn, off) = v;
}

/* COMMAND's half of its dword, leaving STATUS's write-1-to-clear bits be. */
static void command(const struct func *f, uint32_t set, uint32_t clear)
{
    wr(f, COMMAND, ((rd(f, COMMAND) & 0xffff) | set) & ~clear);
}

/* --- BARs. --- */

/* Where the CPU sees a bus address, or 0 if no memory window has it. */
static uint64_t cpu_addr(uint64_t bus, uint64_t size)
{
    for (unsigned i = 0; i < host.ph_NWindows; i++) {
        const struct PciWindow *w = &host.ph_Window[i];

        if (w->pw_Flags & PCI_MEM && bus >= w->pw_Bus && size <= w->pw_Size &&
            bus - w->pw_Bus <= w->pw_Size - size)
            return w->pw_Base + (bus - w->pw_Bus);
    }
    return 0;
}

static void size_bars(struct func *f)
{
    for (unsigned i = 0; i < PCI_BARS; i++) {
        uint32_t off = BAR0 + 4 * i, was = rd(f, off), hi = 0, mask, mhi = ~0u;
        bool b64 = (was & 7) == 4;
        uint64_t bus, size;

        if (was & 1 || (b64 && i == PCI_BARS - 1))
            continue;                   /* I/O space: no use on these machines */
        wr(f, off, ~0u);
        mask = rd(f, off);
        wr(f, off, was);
        if (b64) {
            hi = rd(f, off + 4);
            wr(f, off + 4, ~0u);
            mhi = rd(f, off + 4);
            wr(f, off + 4, hi);
        }
        size = ~((uint64_t)mhi << 32 | (mask & ~0xfu)) + 1;
        if (!(mask & ~0xfu) && !(b64 && mhi))
            size = 0;
        bus = (uint64_t)hi << 32 | (was & ~0xfu);
        if (size) {
            f->f.pf_Bar[i] = (struct PciBar){
                .pb_Base = bus ? cpu_addr(bus, size) : 0, .pb_Size = size,
                .pb_Flags = PCI_MEM | (b64 ? PCI_64 : 0) | (was & 8 ? PCI_PREFETCH : 0),
            };
        }
        if (b64)
            i++;
    }
}

static bool clashes(uint64_t base, uint64_t size)
{
    for (unsigned k = 0; k < nfuncs; k++)
        for (unsigned i = 0; i < PCI_BARS; i++) {
            const struct PciBar *b = &funcs[k].f.pf_Bar[i];

            if (b->pb_Base && b->pb_Size && base < b->pb_Base + b->pb_Size &&
                b->pb_Base < base + size)
                return true;
        }
    return false;
}

/* An address in a window for a BAR the firmware left without one. */
static void assign(struct func *f, unsigned i)
{
    struct PciBar *b = &f->f.pf_Bar[i];

    for (unsigned pass = 0; pass < 2 && !b->pb_Base; pass++)
        for (unsigned w = 0; w < host.ph_NWindows && !b->pb_Base; w++) {
            const struct PciWindow *win = &host.ph_Window[w];
            uint64_t at = (win->pw_Base + b->pb_Size - 1) & ~(b->pb_Size - 1);

            /* Prefetchable 64-bit BARs try a prefetchable window first; a
             * 32-bit BAR never gets a window above 4 GiB on the bus. */
            if (!(win->pw_Flags & PCI_MEM) ||
                (pass == 0 && (win->pw_Flags & PCI_PREFETCH) != (b->pb_Flags & PCI_PREFETCH)) ||
                (!(b->pb_Flags & PCI_64) && win->pw_Bus + win->pw_Size > 1ULL << 32))
                continue;
            for (; at + b->pb_Size <= win->pw_Base + win->pw_Size; at += b->pb_Size)
                if (!clashes(at, b->pb_Size)) {
                    uint64_t bus = win->pw_Bus + (at - win->pw_Base);

                    wr(f, BAR0 + 4 * i, (uint32_t)bus | (rd(f, BAR0 + 4 * i) & 0xf));
                    if (b->pb_Flags & PCI_64)
                        wr(f, BAR0 + 4 * i + 4, (uint32_t)(bus >> 32));
                    b->pb_Base = at;
                    assigned++;
                    break;
                }
        }
}

/* --- Capabilities. --- */

static void read_caps(struct func *f)
{
    uint32_t at = rd(f, CAPS) & 0xfc;

    if (!(rd(f, COMMAND) & STATUS_CAPS))
        return;
    for (unsigned n = 0; at >= 0x40 && n < 48; n++) {
        uint32_t v = rd(f, at);

        if ((v & 0xff) == CAP_MSIX) {
            uint32_t table = rd(f, at + 4);

            f->msix_cap = at;
            f->f.pf_MsixVectors = (v >> 16 & 0x7ff) + 1;
            f->f.pf_MsixBar = table & 7;
            f->f.pf_MsixOffset = table & ~7u;
        }
        at = v >> 8 & 0xfc;
    }
}

/* --- The walk. --- */

/* A PCIe root port's or switch downstream port's link has one device on
 * it, number 0; the others may answer as aliases of it (PCIe 6.0 §7.3.1). */
static bool one_device_below(const struct func *f)
{
    uint32_t at = rd(f, CAPS) & 0xfc;

    if (!(rd(f, COMMAND) & STATUS_CAPS))
        return false;
    for (unsigned n = 0; at >= 0x40 && n < 48; n++) {
        uint32_t v = rd(f, at);

        if ((v & 0xff) == CAP_PCIE) {
            uint32_t type = v >> 20 & 0xf;

            return type == 4 || type == 6;
        }
        at = v >> 8 & 0xfc;
    }
    return false;
}

static void scan(struct drv *d, unsigned bus, unsigned depth, bool only0)
{
    for (unsigned dev = 0; dev < (only0 ? 1u : 32u); dev++)
        for (unsigned fn = 0; fn < 8; fn++) {
            struct func *f = &funcs[nfuncs];
            uint32_t id, header;

            if (nfuncs == MAX_FUNCS)
                return;
            *f = (struct func){ .bus = bus, .dev = dev, .fn = fn };
            id = rd(f, ID);
            if ((id & 0xffff) == 0xffff) {
                if (fn == 0)
                    break;
                continue;
            }
            header = rd(f, HEADER) >> 16 & 0xff;
            f->f = (struct PciFunction){
                .pf_Rid = bus << 8 | dev << 3 | fn, .pf_Vendor = id & 0xffff, .pf_Id = id >> 16,
                .pf_Class = rd(f, CLASS) >> 8, .pf_Revision = rd(f, CLASS) & 0xff,
            };
            f->bridge = (header & 0x7f) == 1;
            if ((header & 0x7f) == 0) {
                command(f, 0, CMD_MEM | CMD_MASTER);    /* no decoding while sized */
                size_bars(f);
                read_caps(f);
            }
            nfuncs++;
            if (f->bridge) {
                uint32_t buses = rd(f, BUSES);
                unsigned sec = buses >> 8 & 0xff, sub = buses >> 16 & 0xff;

                if (sec > bus && sec <= sub && sub <= host.ph_LastBus && depth < 8)
                    scan(d, sec, depth + 1, one_device_below(f));
                else
                    drv_log(d, nullptr, "a bridge the firmware didn't number: not walked");
            }
            if (fn == 0 && !(header & 0x80))
                break;                  /* not a multi-function device */
        }
}

static void put(char *line, unsigned *n, const char *s)
{
    while (*s && *n < 150)
        line[(*n)++] = *s++;
}

static void num(char *line, unsigned *n, uint64_t v)
{
    char dg[24];
    unsigned k = 0;

    do
        dg[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < sizeof dg);
    while (k && *n < 150)
        line[(*n)++] = dg[--k];
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    struct HwdbMsg *m = drv_hwdb_msg(d, HWDB_PCI_HOST);
    unsigned sent = 0, n = 0;
    char line[160];

    if (!m)
        return -10;
    m->hm_Host.ph_Device = (uint32_t)(dev - d->dev);
    if (drv_hwdb(d, m) != 0)
        return -11;
    host = m->hm_Host;
    if (host.ph_FirstBus > host.ph_LastBus || host.ph_LastBus > 255 || host.ph_NWindows > PCI_WINDOWS ||
        (uint64_t)(host.ph_LastBus - host.ph_FirstBus + 1) << 20 > dev->info->dd_Size)
        return -12;
    ecam = (uint64_t)dev->regs;
    nfuncs = assigned = 0;
    scan(d, host.ph_FirstBus, 0, false);

    for (unsigned k = 0; k < nfuncs; k++) {
        struct func *f = &funcs[k];

        for (unsigned i = 0; i < PCI_BARS; i++)
            if (f->f.pf_Bar[i].pb_Size && !f->f.pf_Bar[i].pb_Base)
                assign(f, i);
        for (unsigned i = 0; i < PCI_BARS; i++)
            if (f->f.pf_Bar[i].pb_Size && !f->f.pf_Bar[i].pb_Base) {
                drv_log(d, dev, "a BAR no window can hold: the function is left off");
                f->f.pf_Bar[i].pb_Size = 0;
            }
        if (f->msix_cap && !host.ph_Msi)
            f->f.pf_MsixVectors = 0;    /* nowhere for them to go: the driver polls */

        m = drv_hwdb_msg(d, HWDB_PCI_FUNCTION);
        m->hm_Function = f->f;
        m->hm_Function.pf_Device = (uint32_t)(dev - d->dev);
        if (drv_hwdb(d, m) != 0) {
            drv_log(d, dev, "hwdb refused a function");
            continue;
        }
        sent++;
        if (f->bridge)
            continue;
        if (f->f.pf_MsixVectors) {
            uint32_t c = rd(f, f->msix_cap);

            wr(f, f->msix_cap, (c | MSIX_ENABLE) & ~MSIX_MASKALL);
            command(f, CMD_NOINTX, 0);
        }
        command(f, CMD_MEM | CMD_MASTER, 0);
    }

    put(line, &n, "");
    num(line, &n, sent);
    put(line, &n, " functions on buses ");
    num(line, &n, host.ph_FirstBus);
    put(line, &n, "-");
    num(line, &n, host.ph_LastBus);
    put(line, &n, "; ");
    num(line, &n, assigned);
    put(line, &n, " BARs given addresses");
    line[n] = '\0';
    drv_log(d, dev, line);
    return DRV_OK;
}

static const struct drv_ops ops = { .name = "pci", .bind = bind };

int main(uint64_t token)
{
    drv_main(token, &ops);
}
