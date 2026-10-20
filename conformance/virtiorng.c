/*
 * conformance/virtiorng.c -- a user-mode driver for a virtio entropy
 * device on virtio-mmio (virtio 1.2 §4.2, §5.4)
 *
 * The lease (the first argument's low 32 bits) is granted by its parent,
 * conformance/leaser.c; bits 40-42 say where in its page the device's
 * registers are, in 512-byte steps. The driver maps the registers, sets up one
 * virtqueue in a DMA Area, asks for 64 random bytes in a second one, and
 * waits for the device's interrupt.
 *
 * Mode (bits 32-39) 0: check the bytes came, reset the
 * device, free both Areas and exit 0, or with the number of the first
 * check that failed. Mode 1: once the request is with the device, wait
 * forever with both Areas held, to be killed with DMA outstanding.
 */
#include <planar/exec.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr uint64_t MS = 1000000;
constexpr uint32_t QUEUE = 8;
constexpr uint32_t WANT = 64;

/* virtio-mmio registers. */
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

constexpr uint16_t DESC_WRITE = 2;      /* the device writes this buffer */

struct ring {                           /* the queue's DMA Area */
    struct desc desc[QUEUE];            /* offset 0 */
    uint16_t avail_flags, avail_idx, avail_ring[QUEUE];
    uint8_t pad[256 - sizeof(struct desc) * QUEUE - 4 - 2 * QUEUE];
    uint16_t used_flags;                /* offset 256 */
    volatile uint16_t used_idx;
    struct { uint32_t id, len; } used_ring[QUEUE];
};

static_assert(__builtin_offsetof(struct ring, used_flags) == 256, "the used ring is 4-aligned");

static volatile uint8_t *regs;
static unsigned step;

static int failed(const char *what)
{
    struct logbuf b = { 0 };

    log_str(&b, "virtiorng: check ");
    log_num(&b, step);
    log_str(&b, " failed: ");
    log_str(&b, what);
    KLog(b.text);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(regs + off);
}

static void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(regs + off) = v;
}

static void wr64(uint32_t off, uint64_t v)
{
    wr(off, (uint32_t)v);
    wr(off + 4, (uint32_t)(v >> 32));
}

int main(uint64_t arg)
{
    Handle lease = (Handle)arg;
    unsigned mode = (unsigned)(arg >> 32) & 0xff;
    uint64_t off = (arg >> 40 & 7) * 512;
    struct ring *q;
    uint8_t *buf;
    int64_t qa, ba;
    uint64_t got;
    unsigned nonzero = 0;

    CHECK(MapLease(lease, (void **)&regs) == 0);
    regs += off;
    CHECK(rd(MAGIC) == 0x74726976 && rd(VERSION) == 2 && rd(DEVICE_ID) == 4);
    CHECK(MapLease(lease, (void **)&q) == ERR_EXISTS);
    CHECK(MapFirmware(lease, nullptr) == ERR_BADHANDLE);   /* not the root */

    /* Reset, then the handshake (§3.1.1), offering only VERSION_1. */
    wr(STATUS, 0);
    while (rd(STATUS) != 0)
        ;
    wr(STATUS, ACKNOWLEDGE);
    wr(STATUS, ACKNOWLEDGE | DRIVER);
    wr(DEVICE_FEATURES_SEL, 1);
    CHECK(rd(DEVICE_FEATURES) & 1);             /* VIRTIO_F_VERSION_1 */
    wr(DRIVER_FEATURES_SEL, 0);
    wr(DRIVER_FEATURES, 0);
    wr(DRIVER_FEATURES_SEL, 1);
    wr(DRIVER_FEATURES, 1);
    wr(STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK);
    CHECK(rd(STATUS) & FEATURES_OK);

    /* One queue, its rings in one DMA Area; addresses are physical. */
    qa = AllocDmaArea(lease, 4096, 0, (void **)&q);
    ba = AllocDmaArea(lease, 4096, 0, (void **)&buf);
    CHECK(qa > 0 && ba > 0);
    wr(QUEUE_SEL, 0);
    CHECK(rd(QUEUE_NUM_MAX) >= QUEUE);
    wr(QUEUE_NUM, QUEUE);
    wr64(QUEUE_DESC, (uint64_t)q->desc);
    wr64(QUEUE_DRIVER, (uint64_t)&q->avail_flags);
    wr64(QUEUE_DEVICE, (uint64_t)&q->used_flags);
    wr(QUEUE_READY, 1);
    wr(STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK);

    CHECK(BindIrq(lease, 0, 20) == 0);
    CHECK(BindIrq(lease, 0, 20) == ERR_EXISTS);
    CHECK(BindIrq(lease, 1, 20) == ERR_BADARG);        /* it has one line */
    CHECK(AckWaitIrq(lease, 1, 0) == (uint64_t)ERR_BADARG);

    /* Ask for WANT bytes. */
    q->desc[0] = (struct desc){ .addr = (uint64_t)buf, .len = WANT, .flags = DESC_WRITE };
    q->avail_ring[0] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail_idx = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(QUEUE_NOTIFY, 0);

    if (mode == 1)
        Wait(1ULL << 30);                       /* until killed */

    /* The interrupt, within a second. */
    ArmTimer(CurrentTime() + 1000 * MS, 21);
    do {
        got = AckWaitIrq(lease, 0, 1ULL << 20 | 1ULL << 21);
        CHECK(got != 0 && !(got & 1ULL << 21));
        wr(INTERRUPT_ACK, rd(INTERRUPT_STATUS));
    } while (q->used_idx == 0);
    ArmTimer(0, 0);
    CHECK(AckWaitIrq(lease, 0, 0) == 0);
    CHECK(q->used_ring[0].id == 0 && q->used_ring[0].len == WANT);
    for (unsigned i = 0; i < WANT; i++)
        nonzero += buf[i] != 0;
    CHECK(nonzero > WANT / 2);

    /* Done: stop the device, then give its memory back. */
    wr(STATUS, 0);
    while (rd(STATUS) != 0)
        ;
    CHECK(FreeArea((Handle)qa) == 0 && FreeArea((Handle)ba) == 0);
    return 0;
}
