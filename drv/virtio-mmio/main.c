/*
 * drv/virtio-mmio/main.c -- virtio-mmio slots (virtio 1.2 §4.2), and the
 * entropy (§5.4) and console (§5.3) devices in them
 *
 * One process serves every slot in its lease: QEMU AArch64 puts eight in
 * a page, which can't be leased apart (servers/hwdb/lease.c). Bind
 * declines an empty slot, and any device but these two. Each is reset,
 * given its virtqueues in a DMA Area, and published.
 *
 * The entropy device is "rng.N": a read asks it for up to 64 bytes and
 * waits for its interrupt, a second at most.
 *
 * The first console is "host.0", the host channel (planar/devices/host.h,
 * host-channel plan step 2): port 0 alone, without MULTIPORT, so a
 * receive queue and a transmit queue. Every receive buffer is the
 * device's while it waits for the host; lines are put together from what
 * comes back, kept for HOST_READs, and the buffers given back. A write
 * waits for the device to take its line, a second at most.
 */
#include <planar/devices/host.h>
#include <planar/drv.h>
#include <planar/syscalls.h>

constexpr uint32_t QUEUE = 8;
constexpr uint32_t ENTROPY = 4, CONSOLE = 3;

enum : uint32_t {
    MAGIC = 0x000, VERSION = 0x004, DEVICE_ID = 0x008,
    DEVICE_FEATURES = 0x010, DEVICE_FEATURES_SEL = 0x014,
    DRIVER_FEATURES = 0x020, DRIVER_FEATURES_SEL = 0x024,
    QUEUE_SEL = 0x030, QUEUE_NUM_MAX = 0x034, QUEUE_NUM = 0x038, QUEUE_READY = 0x044,
    QUEUE_NOTIFY = 0x050, INTERRUPT_STATUS = 0x060, INTERRUPT_ACK = 0x064, STATUS = 0x070,
    QUEUE_DESC = 0x080, QUEUE_DRIVER = 0x090, QUEUE_DEVICE = 0x0a0,
};

enum : uint32_t { ACKNOWLEDGE = 1, DRIVER = 2, DRIVER_OK = 4, FEATURES_OK = 8 };

struct desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

constexpr uint16_t DESC_WRITE = 2;

struct ring {                           /* one DMA page: the queue, then the buffer */
    struct desc desc[QUEUE];
    uint16_t avail_flags, avail_idx, avail_ring[QUEUE];
    uint8_t pad[256 - sizeof(struct desc) * QUEUE - 4 - 2 * QUEUE];
    uint16_t used_flags;                /* offset 256 */
    volatile uint16_t used_idx;
    struct { uint32_t id, len; } used_ring[QUEUE];
    uint8_t pad2[512 - 256 - 4 - 8 * QUEUE];
    uint8_t buf[64];                    /* offset 512 */
};

static_assert(__builtin_offsetof(struct ring, used_flags) == 256, "the used ring is 4-aligned");
static_assert(__builtin_offsetof(struct ring, buf) == 512, "the buffer after the rings");

struct rng {
    struct ring *q;
    Handle area;                        /* q's DMA Area */
    uint16_t next;                      /* our avail_idx, and the used_idx we've seen */
};

static struct rng rngs[DRV_DEVICES];

/* --- The console: the host channel (its code is below). --- */

constexpr uint32_t RXBUF = 128;

struct vring {                          /* a queue's rings, laid out as struct ring's */
    struct desc desc[QUEUE];
    uint16_t avail_flags, avail_idx, avail_ring[QUEUE];
    uint8_t pad[256 - sizeof(struct desc) * QUEUE - 4 - 2 * QUEUE];
    uint16_t used_flags;
    volatile uint16_t used_idx;
    struct { uint32_t id, len; } used_ring[QUEUE];
    uint8_t pad2[512 - 256 - 4 - 8 * QUEUE];
};

struct cpage {                          /* one DMA page */
    struct vring rx, tx;                /* queues 0 and 1 */
    uint8_t rxbuf[QUEUE][RXBUF];        /* offset 1024 */
    char txbuf[HOST_LINE + 1];          /* offset 2048 */
};

static_assert(sizeof(struct vring) == 512, "a queue's rings in 512 bytes");
static_assert(sizeof(struct cpage) <= 4096, "the console's queues and buffers in a page");

static struct {
    struct drv_dev *dev;                /* host.0's, once bound */
    struct cpage *q;
    Handle area;
    uint16_t rx_next, rx_seen, tx_next;
    char part[HOST_LINE];               /* a line coming in */
    uint32_t npart;
    char line[HOST_LINES][HOST_LINE];   /* lines come, for reads to come */
    uint32_t len[HOST_LINES];
    unsigned head, count;
    struct { unsigned c; struct HostMsg *m; } reader[DRV_CLIENTS];
    unsigned nreaders;
} con;

static uint32_t rd(const struct drv_dev *dev, uint32_t off)
{
    return *(volatile uint32_t *)(dev->regs + off);
}

static void wr(const struct drv_dev *dev, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(dev->regs + off) = v;
}

static void wr64(const struct drv_dev *dev, uint32_t off, uint64_t v)
{
    wr(dev, off, (uint32_t)v);
    wr(dev, off + 4, (uint32_t)(v >> 32));
}

static int bind_console(struct drv *d, struct drv_dev *dev);

static int bind(struct drv *d, struct drv_dev *dev)
{
    struct rng *r = &rngs[dev - d->dev];
    void *at;

    if (rd(dev, MAGIC) != 0x74726976 || rd(dev, VERSION) != 2 ||
        (rd(dev, DEVICE_ID) != ENTROPY && rd(dev, DEVICE_ID) != CONSOLE))
        return DRV_DECLINE;
    if (dev->info->dd_Line == DRV_NOLINE)
        return DRV_DECLINE;

    /* Reset (whatever a dead driver left), then the handshake (§3.1.1). */
    wr(dev, STATUS, 0);
    while (rd(dev, STATUS) != 0)
        ;
    wr(dev, STATUS, ACKNOWLEDGE);
    wr(dev, STATUS, ACKNOWLEDGE | DRIVER);
    wr(dev, DEVICE_FEATURES_SEL, 1);
    if (!(rd(dev, DEVICE_FEATURES) & 1))        /* VIRTIO_F_VERSION_1 */
        return DRV_DECLINE;
    wr(dev, DRIVER_FEATURES_SEL, 0);
    wr(dev, DRIVER_FEATURES, 0);
    wr(dev, DRIVER_FEATURES_SEL, 1);
    wr(dev, DRIVER_FEATURES, 1);
    wr(dev, STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK);
    if (!(rd(dev, STATUS) & FEATURES_OK))
        return -10;
    if (rd(dev, DEVICE_ID) == CONSOLE)
        return bind_console(d, dev);

    int64_t area = AllocDmaArea(drv_lease(d), 4096, 0, &at);

    if (area < 0)
        return -11;
    r->area = (Handle)area;
    r->q = at;
    r->next = 0;
    wr(dev, QUEUE_SEL, 0);
    if (rd(dev, QUEUE_NUM_MAX) < QUEUE)
        return -12;
    wr(dev, QUEUE_NUM, QUEUE);
    wr64(dev, QUEUE_DESC, (uint64_t)r->q->desc);
    wr64(dev, QUEUE_DRIVER, (uint64_t)&r->q->avail_flags);
    wr64(dev, QUEUE_DEVICE, (uint64_t)&r->q->used_flags);
    wr(dev, QUEUE_READY, 1);
    wr(dev, STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK);
    return DRV_OK;
}

static int start(struct drv *d, struct drv_dev *dev)
{
    return (int)(dev == con.dev ? drv_publish_as(d, dev, "host.0") : drv_publish(d, dev, "rng"));
}

static void request(struct drv *d, struct drv_dev *dev, struct DrvRequest *req)
{
    struct rng *r = &rngs[dev - d->dev];
    struct ring *q = r->q;
    uint16_t slot = r->next % QUEUE;
    uint32_t want = req->dr_Len ? req->dr_Len : sizeof req->dr_Data, got;

    if (req->dr_Op != DRV_READ) {
        req->dr_Msg.mn_Error = (int32_t)ERR_BADARG;
        return;
    }
    if (want > sizeof q->buf)
        want = sizeof q->buf;
    q->desc[slot] = (struct desc){ .addr = (uint64_t)q->buf, .len = want, .flags = DESC_WRITE };
    q->avail_ring[slot] = slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail_idx = ++r->next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(dev, QUEUE_NOTIFY, 0);

    while (q->used_idx != r->next) {
        uint64_t sig = drv_wait_irq(d, dev, 1000);

        wr(dev, INTERRUPT_ACK, rd(dev, INTERRUPT_STATUS));
        if (!(sig & 1ULL << (32 + dev->info->dd_Line)) && q->used_idx != r->next) {
            req->dr_Msg.mn_Error = (int32_t)ERR_BUSY;     /* a second and nothing */
            req->dr_Len = 0;
            return;
        }
    }
    got = q->used_ring[slot].len;
    if (got > want)
        got = want;
    for (unsigned i = 0; i < got; i++)
        req->dr_Data[i] = q->buf[i];
    req->dr_Len = got;
    req->dr_Msg.mn_Error = 0;
}

/* Asked to stop: a reset, after which the device touches no memory
 * (virtio 1.2 §4.2.2.1), then its queue's Area back. */
static void stop(struct drv *d, struct drv_dev *dev)
{
    struct rng *r = &rngs[dev - d->dev];
    Handle *area = dev == con.dev ? &con.area : &r->area;

    wr(dev, STATUS, 0);
    while (rd(dev, STATUS) != 0)
        ;
    if (*area)
        FreeArea(*area);
    *area = 0;
}

/* --- The console. --- */

/* Every receive buffer given to the device that isn't. */
static void rx_give(void)
{
    struct cpage *q = con.q;

    while ((uint16_t)(con.rx_next - con.rx_seen) < QUEUE) {
        uint16_t slot = con.rx_next % QUEUE;

        q->rx.desc[slot] = (struct desc){ .addr = (uint64_t)q->rxbuf[slot], .len = RXBUF, .flags = DESC_WRITE };
        q->rx.avail_ring[slot] = slot;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        q->rx.avail_idx = ++con.rx_next;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(con.dev, QUEUE_NOTIFY, 0);
}

/* Readers answered from the lines kept, oldest first. */
static void answer(struct drv *d)
{
    while (con.nreaders && con.count) {
        struct HostMsg *m = con.reader[0].m;
        unsigned c = con.reader[0].c, k = con.head % HOST_LINES;

        for (uint32_t i = 0; i < con.len[k]; i++)
            m->hm_Text[i] = con.line[k][i];
        m->hm_Len = con.len[k];
        m->hm_Msg.mn_Error = 0;
        con.head++;
        con.count--;
        for (unsigned i = 1; i < con.nreaders; i++)
            con.reader[i - 1] = con.reader[i];
        con.nreaders--;
        drv_reply(d, c, &m->hm_Msg);
    }
}

/* What the host sent, into lines; the buffers given back. */
static void rx_take(struct drv *d)
{
    struct cpage *q = con.q;

    while (con.rx_seen != q->rx.used_idx) {
        uint16_t slot = con.rx_seen % QUEUE;
        uint32_t n = q->rx.used_ring[slot].len;

        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (n > RXBUF)
            n = RXBUF;
        for (uint32_t i = 0; i < n; i++) {
            char ch = (char)q->rxbuf[slot][i];

            if (ch == '\r')
                continue;
            if (ch != '\n') {
                if (con.npart < HOST_LINE)
                    con.part[con.npart++] = ch;
                continue;
            }
            if (con.count == HOST_LINES) {      /* nobody reading: the oldest goes */
                con.head++;
                con.count--;
            }
            unsigned k = (con.head + con.count) % HOST_LINES;

            for (uint32_t j = 0; j < con.npart; j++)
                con.line[k][j] = con.part[j];
            con.len[k] = con.npart;
            con.count++;
            con.npart = 0;
        }
        con.rx_seen++;
    }
    rx_give();
    answer(d);
}

/* A line to the host, and a newline: once the device has it, a second at most. */
static int64_t tx_line(struct drv *d, const char *text, uint32_t n)
{
    struct cpage *q = con.q;
    uint16_t slot = con.tx_next % QUEUE;
    uint64_t until = CurrentTime() + 1000000000ULL;
    bool took = false;

    for (uint32_t i = 0; i < n; i++)
        q->txbuf[i] = text[i];
    q->txbuf[n] = '\n';
    q->tx.desc[slot] = (struct desc){ .addr = (uint64_t)q->txbuf, .len = n + 1, .flags = 0 };
    q->tx.avail_ring[slot] = slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->tx.avail_idx = ++con.tx_next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(con.dev, QUEUE_NOTIFY, 1);
    while (q->tx.used_idx != con.tx_next && CurrentTime() <= until) {
        took |= drv_wait_irq(d, con.dev, 100) & 1ULL << (32 + con.dev->info->dd_Line);
        wr(con.dev, INTERRUPT_ACK, rd(con.dev, INTERRUPT_STATUS));
    }
    /* A signal of the line's taken here is the loop's no more: the line
     * given back, or the host's answer would never interrupt. */
    if (took)
        AckWaitIrq(drv_lease(d), con.dev->info->dd_Line, 0);
    rx_take(d);
    return q->tx.used_idx == con.tx_next ? 0 : ERR_TIMEOUT;
}

/* Queue 'i' of 'dev' on the rings at 'v'. */
static bool queue_on(const struct drv_dev *dev, uint32_t i, struct vring *v)
{
    wr(dev, QUEUE_SEL, i);
    if (rd(dev, QUEUE_NUM_MAX) < QUEUE)
        return false;
    wr(dev, QUEUE_NUM, QUEUE);
    wr64(dev, QUEUE_DESC, (uint64_t)v->desc);
    wr64(dev, QUEUE_DRIVER, (uint64_t)&v->avail_flags);
    wr64(dev, QUEUE_DEVICE, (uint64_t)&v->used_flags);
    wr(dev, QUEUE_READY, 1);
    return true;
}

static int bind_console(struct drv *d, struct drv_dev *dev)
{
    void *at;
    int64_t area;

    if (con.dev)
        return DRV_DECLINE;             /* one host channel */
    if ((area = AllocDmaArea(drv_lease(d), 4096, 0, &at)) < 0)
        return -11;
    con.area = (Handle)area;
    con.q = at;
    if (!queue_on(dev, 0, &con.q->rx) || !queue_on(dev, 1, &con.q->tx))
        return -12;
    wr(dev, STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK);
    con.dev = dev;
    rx_give();
    return DRV_OK;
}

static void host_message(struct drv *d, unsigned c, struct HostMsg *m)
{
    uint32_t op = m->hm_Op, n = m->hm_Len;
    char text[HOST_LINE];

    if (m->hm_Msg.mn_Length < sizeof *m || !drv_in_heap(d, c, (uint64_t)m, sizeof *m)) {
        m->hm_Msg.mn_Error = (int32_t)ERR_BADARG;
    } else if (op == HOST_WRITE) {
        if (n > HOST_LINE)
            n = HOST_LINE;
        for (uint32_t i = 0; i < n; i++)  /* the client's memory: copied once */
            text[i] = m->hm_Text[i];
        m->hm_Msg.mn_Error = n ? (int32_t)tx_line(d, text, n) : 0;
    } else if (op == HOST_READ) {
        for (unsigned i = 0; i < con.nreaders; i++)
            if (con.reader[i].c == c) {
                m->hm_Msg.mn_Error = (int32_t)ERR_BUSY;
                drv_reply(d, c, &m->hm_Msg);
                return;
            }
        con.reader[con.nreaders].c = c;
        con.reader[con.nreaders++].m = m;
        answer(d);
        return;                         /* answered with a line */
    } else {
        m->hm_Msg.mn_Error = (int32_t)ERR_BADARG;
    }
    drv_reply(d, c, &m->hm_Msg);
}

/* --- Both. --- */

static void message(struct drv *d, struct drv_dev *dev, unsigned c, struct Message *m)
{
    struct DrvRequest *q = (struct DrvRequest *)m, r;

    if (dev == con.dev) {
        host_message(d, c, (struct HostMsg *)m);
        return;
    }
    memcpy(&r, q, sizeof r);            /* the client's memory: copied once */
    if (r.dr_Len > sizeof r.dr_Data)
        r.dr_Len = sizeof r.dr_Data;
    request(d, dev, &r);
    q->dr_Len = r.dr_Len;
    memcpy(q->dr_Data, r.dr_Data, sizeof q->dr_Data);
    q->dr_Msg.mn_Error = r.dr_Msg.mn_Error;
    drv_reply(d, c, m);
}

static void interrupt(struct drv *d, struct drv_dev *dev)
{
    if (dev != con.dev)
        return;                         /* the entropy device's: its request waits for it */
    wr(dev, INTERRUPT_ACK, rd(dev, INTERRUPT_STATUS));
    rx_take(d);
}

/* A client gone: its read, if any, answered (into a connection that's
 * gone) and forgotten. */
static void gone(struct drv *d, struct drv_dev *dev, unsigned c)
{
    if (dev != con.dev)
        return;
    for (unsigned i = 0; i < con.nreaders;)
        if (con.reader[i].c == c) {
            struct HostMsg *m = con.reader[i].m;

            for (unsigned k = i + 1; k < con.nreaders; k++)
                con.reader[k - 1] = con.reader[k];
            con.nreaders--;
            m->hm_Msg.mn_Error = (int32_t)ERR_PORT_DIED;
            drv_reply(d, c, &m->hm_Msg);
        } else {
            i++;
        }
}

static const struct drv_ops ops = {
    .name = "virtio-mmio", .bind = bind, .start = start, .interrupt = interrupt, .message = message,
    .message_size = sizeof(struct DrvRequest), .gone = gone, .stop = stop,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
