/*
 * lib/drv/drv.c -- drv.library: a driver's main loop (docs §11.7)
 *
 * planar/drv.h says what it does. Signals: 16-19 are the library's, and
 * line n of the first lease is signal 32 + n, so a lease's 32 lines each
 * have one. A driver is one task.
 */
#include <stddef.h>

#include <planar/drv.h>
#include <planar/syscalls.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_HWDB = 16;        /* hwdb's connection */
constexpr int64_t SIG_ACCEPT = 17;      /* a client at a class port */
constexpr int64_t SIG_CLIENT = 18;      /* a client's connection */
constexpr int64_t SIG_TIMER = 19;
constexpr int64_t SIG_LINE0 = 32;
constexpr uint64_t MS = 1000000;

static struct drv the;

/*
 * Clients. The live connections are kept packed, for WaitPorts; each has
 * a number that stays its own (drv_reply's), and a client that went with
 * messages still unanswered moves out of the live ones until it's
 * answered them all, then closes.
 */
static struct MsgPort live[DRV_CLIENTS];
static uint8_t live_id[DRV_CLIENTS];
static unsigned nlive;
static struct {
    bool used;
    int pos;                    /* in live[], or -1 once gone */
    uint8_t dev;
    unsigned pending;           /* messages given the driver, not yet answered */
    struct MsgPort gone;        /* its connection, once gone */
} client[DRV_CLIENTS];

static struct MsgPort *port_of(unsigned c)
{
    return client[c].pos >= 0 ? &live[client[c].pos] : &client[c].gone;
}

bool drv_in_heap(const struct drv *, unsigned c, uint64_t addr, uint64_t size)
{
    const struct MsgPort *mp;

    if (c >= DRV_CLIENTS || !client[c].used)
        return false;
    mp = port_of(c);
    return addr >= mp->mp_HeapLo && addr <= mp->mp_HeapHi && size <= mp->mp_HeapHi - addr;
}

void drv_reply(struct drv *, unsigned c, struct Message *m)
{
    if (c >= DRV_CLIENTS || !client[c].used || client[c].pending == 0)
        return;
    client[c].pending--;
    if (client[c].pos >= 0) {
        ReplyMsgLater(&live[client[c].pos], m);
    } else if (client[c].pending == 0) {
        ClosePort(&client[c].gone);
        client[c].used = false;
    }
}

/* Take live[pos] out of the packed ones. */
static void unlive(unsigned pos)
{
    unsigned c = live_id[pos];

    client[c].pos = -1;
    if (--nlive != pos) {
        live[pos] = live[nlive];
        live_id[pos] = live_id[nlive];
        client[live_id[pos]].pos = (int)pos;
    }
}

void drv_log(const struct drv *d, const struct drv_dev *dev, const char *text)
{
    char line[160];
    unsigned n = 0;

    for (const char *s = d->ops->name; *s && n < 60; s++)
        line[n++] = *s;
    if (dev) {
        line[n++] = '.';
        unsigned u = dev->info->dd_Unit;
        char digits[8];
        unsigned k = 0;

        do
            digits[k++] = (char)('0' + u % 10);
        while ((u /= 10) && k < sizeof digits);
        while (k)
            line[n++] = digits[--k];
    }
    line[n++] = ':';
    line[n++] = ' ';
    for (; *text && n < sizeof line - 1; text++)
        line[n++] = *text;
    line[n] = '\0';
    KLog(line);
}

static struct HwdbMsg *spare;

struct HwdbMsg *drv_hwdb_msg(struct drv *d, uint32_t op)
{
    if (!spare && !(spare = AllocMsg(&d->hwdb, sizeof *spare)))
        return nullptr;
    memset(spare, 0, sizeof *spare);
    spare->hm_Msg.mn_Length = sizeof *spare;
    spare->hm_Op = op;
    spare->hm_Token = d->token;
    return spare;
}

int64_t drv_hwdb(struct drv *d, struct HwdbMsg *m)
{
    int64_t e = DoMsg(&d->hwdb, &m->hm_Msg);

    return e ? e : m->hm_Msg.mn_Error;
}

Handle drv_lease(const struct drv *d)
{
    return d->info.hm_Range[0].dr_Lease;
}

uint64_t drv_wait_irq(struct drv *d, struct drv_dev *dev, uint64_t ms)
{
    uint64_t mask = 1ULL << (SIG_LINE0 + dev->info->dd_Line), got;

    if (ms) {
        ArmTimer(CurrentTime() + ms * MS, SIG_TIMER);
        mask |= 1ULL << SIG_TIMER;
    }
    got = AckWaitIrq(drv_lease(d), dev->info->dd_Line, mask);
    if (ms)
        ArmTimer(0, 0);
    return got;
}

/* The next of 'dev's ports, named 'name' (at most 15 bytes). */
static int64_t publish(struct drv_dev *dev, const char *name)
{
    unsigned p = 0, n = 0;
    int64_t h;

    while (p < DRV_PORTS && dev->port[p])
        p++;
    if (p == DRV_PORTS)
        return ERR_FULL;
    for (; *name && n < sizeof dev->port_name[p] - 1; name++)
        dev->port_name[p][n++] = *name;
    dev->port_name[p][n] = '\0';
    if ((h = CreatePort(dev->port_name[p], SIG_ACCEPT)) < 0) {
        dev->port_name[p][0] = '\0';
        return h;
    }
    dev->port[p] = (Handle)h;
    return 0;
}

int64_t drv_publish(struct drv *, struct drv_dev *dev, const char *cls)
{
    unsigned n = 0, u = dev->info->dd_Unit, k = 0;
    char name[16], digits[8];

    for (; *cls && n < sizeof name - 10; cls++)
        name[n++] = *cls;
    name[n++] = '.';
    do
        digits[k++] = (char)('0' + u % 10);
    while ((u /= 10) && k < sizeof digits);
    while (k)
        name[n++] = digits[--k];
    name[n] = '\0';
    return publish(dev, name);
}

int64_t drv_publish_as(struct drv *, struct drv_dev *dev, const char *name)
{
    return publish(dev, name);
}

/* Copy hwdb's answer in and check it: it's hwdb's, but in shared memory.
 * A device with no registers (QEMU's power model) has none in a range. */
static bool take_info(struct drv *d, const struct HwdbMsg *m)
{
    memcpy(&d->info, m, sizeof d->info);
    if (d->info.hm_NRanges > DRV_RANGES || d->info.hm_NDevices > DRV_DEVICES)
        return false;
    d->ndev = d->info.hm_NDevices;
    for (unsigned i = 0; i < d->ndev; i++) {
        const struct DrvDevice *dd = &d->info.hm_Device[i];
        bool inside = dd->dd_Size == 0 && dd->dd_Line == DRV_NOLINE;

        d->info.hm_Device[i].dd_Key[sizeof dd->dd_Key - 1] = '\0';
        for (unsigned r = 0; r < d->info.hm_NRanges; r++) {
            const struct DrvRange *rg = &d->info.hm_Range[r];

            if (dd->dd_Base >= rg->dr_Base && dd->dd_Size <= rg->dr_Size &&
                dd->dd_Base - rg->dr_Base <= rg->dr_Size - dd->dd_Size)
                inside = true;
        }
        if (!inside || (dd->dd_Line != DRV_NOLINE && dd->dd_Line >= LEASE_IRQS) ||
            dd->dd_NLines > LEASE_IRQS || (dd->dd_NLines && dd->dd_Line + dd->dd_NLines > LEASE_IRQS))
            return false;
        d->dev[i].info = dd;
    }
    return true;
}

static void serve(struct drv *d, unsigned pos)
{
    unsigned c = live_id[pos];
    struct drv_dev *dev = &d->dev[client[c].dev];
    struct DrvRequest *m;

    if (d->ops->message) {
        struct Message *msg;
        uint32_t size = d->ops->message_size < sizeof *msg ? sizeof *msg : d->ops->message_size;

        while (client[c].pos >= 0 && (msg = GetMsgSized(&live[client[c].pos], size))) {
            client[c].pending++;
            d->ops->message(d, dev, c, msg);
        }
        return;
    }
    while ((m = (struct DrvRequest *)GetMsgSized(&live[pos], sizeof *m))) {
        struct DrvRequest r;

        memcpy(&r, m, sizeof r);
        if (r.dr_Len > sizeof r.dr_Data)
            r.dr_Len = sizeof r.dr_Data;
        if (d->ops->request) {
            d->ops->request(d, dev, &r);
        } else {
            r.dr_Msg.mn_Error = (int32_t)ERR_BADARG;
        }
        m->dr_Len = r.dr_Len;
        memcpy(m->dr_Data, r.dr_Data, sizeof m->dr_Data);
        m->dr_Msg.mn_Error = r.dr_Msg.mn_Error;
        ReplyMsgLater(&live[pos], &m->dr_Msg);
    }
}

[[noreturn]] void drv_main(uint64_t token, const struct drv_ops *ops)
{
    struct drv *d = &the;
    struct HwdbMsg *m;
    uint64_t lines = 0;
    unsigned taken = 0;

    d->ops = ops;
    d->token = (uint32_t)token;
    if (ConnectPort(&d->hwdb, "hwdb", SIG_HWDB, 4, 3 * sizeof *m) != 0 ||
        !(m = AllocMsg(&d->hwdb, sizeof *m)))
        ExitProcess(-1);
    memset(m, 0, sizeof *m);
    m->hm_Msg.mn_Length = sizeof *m;
    m->hm_Op = HWDB_BIND;
    m->hm_Token = (uint32_t)token;
    if (DoMsg(&d->hwdb, &m->hm_Msg) != 0 || m->hm_Msg.mn_Error != 0 || !take_info(d, m))
        ExitProcess(-2);

    /* The registers: each range is mapped where it is. */
    for (unsigned r = 0; r < d->info.hm_NRanges; r++) {
        void *at;

        if (MapLease(d->info.hm_Range[r].dr_Lease, &at) != 0)
            ExitProcess(-3);
    }
    for (unsigned i = 0; i < d->ndev; i++)
        d->dev[i].regs = (volatile uint8_t *)d->dev[i].info->dd_Base;
    if (d->info.hm_Flags & DRVF_CRASH)
        __builtin_trap();

    for (unsigned i = 0; i < d->ndev; i++) {
        struct drv_dev *dev = &d->dev[i];
        int rc = ops->bind(d, dev);

        if (rc < 0)
            ExitProcess(rc);
        dev->accepted = rc == DRV_OK;
        taken += dev->accepted;
        if (dev->accepted && dev->info->dd_Line != DRV_NOLINE) {
            /* Its line, or its MSI-X vectors' lines from there. */
            for (uint32_t k = 0; k < (dev->info->dd_NLines ? dev->info->dd_NLines : 1); k++) {
                uint32_t line = dev->info->dd_Line + k;

                if (line >= LEASE_IRQS || BindIrq(drv_lease(d), line, SIG_LINE0 + line) != 0)
                    ExitProcess(-4);
                lines |= 1ULL << (SIG_LINE0 + line);
            }
            dev->line_bound = true;
        }
    }
    for (unsigned i = 0; i < d->ndev; i++)
        if (d->dev[i].accepted && ops->start && ops->start(d, &d->dev[i]) < 0)
            ExitProcess(-5);

    /* Tell hwdb what came of it. */
    memset(m, 0, sizeof *m);
    m->hm_Msg.mn_Length = sizeof *m;
    m->hm_Op = HWDB_STARTED;
    m->hm_Token = (uint32_t)token;
    for (unsigned i = 0; i < d->ndev; i++) {
        if (d->dev[i].accepted)
            m->hm_Accepted |= 1u << i;
        memcpy(m->hm_Ports[i], d->dev[i].port_name[0], sizeof m->hm_Ports[i]);
    }
    if (DoMsg(&d->hwdb, &m->hm_Msg) != 0)
        ExitProcess(-6);
    if (!taken)
        ExitProcess(0);

    for (;;) {
        uint64_t want = lines | 1ULL << SIG_ACCEPT | 1ULL << SIG_CLIENT | 1ULL << SIG_HWDB, got;

        uint64_t until = ops->deadline ? ops->deadline(d) : 0;

        if (ops->poll && ops->poll(d)) {
            /* Work outstanding on a polled device: look, but don't sleep. */
            for (unsigned i = 0; i < nlive; i++)
                FlushPort(&live[i]);
            got = want;
        } else {
            if (until)
                ArmTimer(until, SIG_TIMER);
            got = WaitPorts(live, nlive, want | (until ? 1ULL << SIG_TIMER : 0));
            if (until)
                ArmTimer(0, 0);
        }
        if (PortGone(&d->hwdb))
            ExitProcess(0);
        if (got & 1ULL << SIG_ACCEPT)
            for (unsigned i = 0; i < d->ndev * DRV_PORTS; i++)
                while (d->dev[i / DRV_PORTS].port[i % DRV_PORTS]) {
                    struct MsgPort mp;
                    unsigned c = 0;

                    if (AcceptPort(&mp, d->dev[i / DRV_PORTS].port[i % DRV_PORTS], SIG_CLIENT) != 0)
                        break;
                    while (c < DRV_CLIENTS && client[c].used)
                        c++;
                    if (c == DRV_CLIENTS) {
                        ClosePort(&mp);
                        continue;
                    }
                    client[c].used = true;
                    client[c].dev = (uint8_t)(i / DRV_PORTS);
                    client[c].pending = 0;
                    client[c].pos = (int)nlive;
                    live_id[nlive] = (uint8_t)c;
                    live[nlive++] = mp;
                }
        for (unsigned i = 0; i < d->ndev; i++) {
            struct drv_dev *dev = &d->dev[i];
            uint32_t n = dev->info->dd_NLines ? dev->info->dd_NLines : 1;
            uint64_t mine = 0;

            if (!dev->line_bound)
                continue;
            for (uint32_t k = 0; k < n; k++)
                mine |= 1ULL << (SIG_LINE0 + dev->info->dd_Line + k);
            if (!(got & mine))
                continue;
            if (ops->interrupt)
                ops->interrupt(d, dev);
            for (uint32_t k = 0; k < n; k++)
                if (got & 1ULL << (SIG_LINE0 + dev->info->dd_Line + k))
                    AckWaitIrq(drv_lease(d), dev->info->dd_Line + k, 0);
        }
        for (unsigned pos = 0; pos < nlive;) {
            unsigned c = live_id[pos];

            serve(d, pos);
            if (!PortGone(&live[pos])) {
                pos++;
                continue;
            }
            if (d->ops->gone)
                d->ops->gone(d, &d->dev[client[c].dev], c);
            if (client[c].pending == 0) {
                ClosePort(&live[pos]);
                client[c].used = false;
            } else {
                client[c].gone = live[pos];     /* until its answers are given */
            }
            unlive(pos);
        }
    }
}
