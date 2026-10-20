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
static struct MsgPort clients[DRV_CLIENTS];
static uint8_t client_dev[DRV_CLIENTS];
static unsigned nclients;

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

int64_t drv_publish(struct drv *, struct drv_dev *dev, const char *cls)
{
    unsigned n = 0, u = dev->info->dd_Unit, k = 0;
    char digits[8];
    int64_t h;

    for (; *cls && n < sizeof dev->port_name - 10; cls++)
        dev->port_name[n++] = *cls;
    dev->port_name[n++] = '.';
    do
        digits[k++] = (char)('0' + u % 10);
    while ((u /= 10) && k < sizeof digits);
    while (k)
        dev->port_name[n++] = digits[--k];
    dev->port_name[n] = '\0';
    if ((h = CreatePort(dev->port_name, SIG_ACCEPT)) < 0)
        return h;
    dev->port = (Handle)h;
    return 0;
}

/* Copy hwdb's answer in and check it: it's hwdb's, but in shared memory. */
static bool take_info(struct drv *d, const struct HwdbMsg *m)
{
    memcpy(&d->info, m, sizeof d->info);
    if (d->info.hm_NRanges == 0 || d->info.hm_NRanges > DRV_RANGES ||
        d->info.hm_NDevices > DRV_DEVICES)
        return false;
    d->ndev = d->info.hm_NDevices;
    for (unsigned i = 0; i < d->ndev; i++) {
        const struct DrvDevice *dd = &d->info.hm_Device[i];
        bool inside = false;

        d->info.hm_Device[i].dd_Key[sizeof dd->dd_Key - 1] = '\0';
        for (unsigned r = 0; r < d->info.hm_NRanges; r++) {
            const struct DrvRange *rg = &d->info.hm_Range[r];

            if (dd->dd_Base >= rg->dr_Base && dd->dd_Size <= rg->dr_Size &&
                dd->dd_Base - rg->dr_Base <= rg->dr_Size - dd->dd_Size)
                inside = true;
        }
        if (!inside || (dd->dd_Line != DRV_NOLINE && dd->dd_Line >= LEASE_IRQS))
            return false;
        d->dev[i].info = dd;
    }
    return true;
}

static void serve(struct drv *d, unsigned c)
{
    struct drv_dev *dev = &d->dev[client_dev[c]];
    struct DrvRequest *m;

    while ((m = (struct DrvRequest *)GetMsgSized(&clients[c], sizeof *m))) {
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
        ReplyMsgLater(&clients[c], &m->dr_Msg);
    }
}

[[noreturn]] void drv_main(uint64_t token, const struct drv_ops *ops)
{
    struct drv *d = &the;
    struct HwdbMsg *m;
    uint64_t lines = 0;
    unsigned taken = 0;

    d->ops = ops;
    if (ConnectPort(&d->hwdb, "hwdb", SIG_HWDB, 4, 4096) != 0 ||
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
            if (BindIrq(drv_lease(d), dev->info->dd_Line, SIG_LINE0 + dev->info->dd_Line) != 0)
                ExitProcess(-4);
            dev->line_bound = true;
            lines |= 1ULL << (SIG_LINE0 + dev->info->dd_Line);
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
        memcpy(m->hm_Ports[i], d->dev[i].port_name, sizeof m->hm_Ports[i]);
    }
    if (DoMsg(&d->hwdb, &m->hm_Msg) != 0)
        ExitProcess(-6);
    if (!taken)
        ExitProcess(0);

    for (;;) {
        uint64_t got = WaitPorts(clients, nclients,
                                 lines | 1ULL << SIG_ACCEPT | 1ULL << SIG_CLIENT | 1ULL << SIG_HWDB);

        if (PortGone(&d->hwdb))
            ExitProcess(0);
        if (got & 1ULL << SIG_ACCEPT)
            for (unsigned i = 0; i < d->ndev; i++)
                while (d->dev[i].port) {
                    struct MsgPort mp;

                    if (AcceptPort(&mp, d->dev[i].port, SIG_CLIENT) != 0)
                        break;
                    if (nclients == DRV_CLIENTS) {
                        ClosePort(&mp);
                        continue;
                    }
                    client_dev[nclients] = (uint8_t)i;
                    clients[nclients++] = mp;
                }
        for (unsigned i = 0; i < d->ndev; i++) {
            struct drv_dev *dev = &d->dev[i];

            if (!dev->line_bound || !(got & 1ULL << (SIG_LINE0 + dev->info->dd_Line)))
                continue;
            if (ops->interrupt)
                ops->interrupt(d, dev);
            AckWaitIrq(drv_lease(d), dev->info->dd_Line, 0);
        }
        for (unsigned c = 0; c < nclients;) {
            serve(d, c);
            if (PortGone(&clients[c])) {
                ClosePort(&clients[c]);
                clients[c] = clients[--nclients];
                client_dev[c] = client_dev[nclients];
            } else {
                c++;
            }
        }
    }
}
