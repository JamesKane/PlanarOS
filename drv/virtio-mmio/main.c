/*
 * drv/virtio-mmio/main.c -- virtio-mmio slots (virtio 1.2 §4.2), and the
 * entropy device in them (§5.4)
 *
 * One process serves every slot in its lease: QEMU AArch64 puts eight in
 * a page, which can't be leased apart (servers/hwdb/lease.c). Bind
 * declines an empty slot, and any device but the entropy one so far; for
 * that it resets the device, sets up one virtqueue in a DMA Area, and
 * publishes "rng.N". A read asks the device for up to 64 bytes and waits
 * for its interrupt, a second at most.
 */
#include <planar/drv.h>
#include <planar/syscalls.h>

constexpr uint32_t QUEUE = 8;
constexpr uint32_t ENTROPY = 4;

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
    uint16_t next;                      /* our avail_idx, and the used_idx we've seen */
};

static struct rng rngs[DRV_DEVICES];

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

static int bind(struct drv *d, struct drv_dev *dev)
{
    struct rng *r = &rngs[dev - d->dev];
    void *at;

    if (rd(dev, MAGIC) != 0x74726976 || rd(dev, VERSION) != 2 || rd(dev, DEVICE_ID) != ENTROPY)
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

    if (AllocDmaArea(drv_lease(d), 4096, 0, &at) < 0)
        return -11;
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
    return (int)drv_publish(d, dev, "rng");
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

static const struct drv_ops ops = {
    .name = "virtio-mmio", .bind = bind, .start = start, .request = request,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
