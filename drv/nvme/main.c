/*
 * drv/nvme/main.c -- NVMe controllers (NVM Express Base 2.0, NVM Command
 * Set 1.0), found by pci (§11.6) by their class, 010802
 *
 * Bind resets the controller (whatever a dead driver left), sets up the
 * admin queue, identifies the controller and namespace 1, and makes one
 * I/O queue pair. Its queues and PRP lists are in DMA Areas, so they
 * are quarantined if it dies, and hwdb stops the controller (CC.EN = 0)
 * before letting them go (servers/hwdb/server.c).
 *
 * It serves the block class (planar/devices/block.h) on "nvme.N": each
 * request becomes one command, reading or writing the client's buffer
 * where it lies in the connection's heap, through a PRP list when it
 * spans more than two pages. A message is answered once all its
 * requests are; when the queue is full, messages wait their turn here.
 *
 * Completions come by MSI-X when hwdb gave it vectors (dd_NLines):
 * vector 0 is the admin queue's, the last the I/O queue's. Else it
 * polls, and drv.library doesn't sleep while commands are outstanding.
 */
#include <planar/devices/block.h>
#include <planar/drv.h>
#include <planar/syscalls.h>

constexpr uint32_t ADMIN_DEPTH = 32;
constexpr uint32_t IO_DEPTH = 64;               /* a slot is left empty: 63 in flight */
constexpr uint32_t BACKLOG = 64;                /* messages waiting for slots */
constexpr uint32_t MAX_BYTES = 128 * 1024;      /* per command; fits a 32-entry PRP list */
constexpr uint32_t PRP_ENTRIES = MAX_BYTES / 4096 + 1;
constexpr uint64_t PAGE = 4096;
constexpr uint64_t MS = 1000000;

enum : uint32_t {
    CAP = 0x00, VS = 0x08, INTMS = 0x0c, CC = 0x14, CSTS = 0x1c, AQA = 0x24, ASQ = 0x28,
    ACQ = 0x30, DOORBELLS = 0x1000,
};
enum : uint32_t { CC_EN = 1u << 0, CSTS_RDY = 1u << 0, CSTS_CFS = 1u << 1 };
enum : uint8_t {
    ADM_CREATE_SQ = 0x01, ADM_CREATE_CQ = 0x05, ADM_IDENTIFY = 0x06, ADM_SET_FEATURES = 0x09,
    IO_FLUSH = 0x00, IO_WRITE = 0x01, IO_READ = 0x02,
};

struct sqe {
    uint32_t cdw0;                      /* opcode, then the command ID in 31:16 */
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cqe {
    uint32_t dw0, dw1;
    uint16_t sqhd, sqid;
    uint16_t cid;
    volatile uint16_t status;           /* bit 0 is the phase */
};

static_assert(sizeof(struct sqe) == 64, "a submission entry is 64 bytes");
static_assert(sizeof(struct cqe) == 16, "a completion entry is 16 bytes");

struct queue {
    struct sqe *sq;
    struct cqe *cq;
    uint32_t depth, qid;
    uint32_t tail, head;                /* our SQ tail, our CQ head */
    uint16_t phase, cid;
};

/* A message being served: its requests, each a command in a slot. */
struct inflight {
    struct BlkMsg *m;                   /* the client's: written, never read again */
    unsigned client;
    unsigned next;                      /* its next request to submit */
    unsigned count;
    unsigned left;                      /* submitted or not, not yet complete */
    bool used;
};

struct slot {
    bool busy;
    uint16_t msg;                       /* its inflight */
    uint16_t req;
    uint64_t *prp;                      /* its PRP list, in a DMA Area */
};

struct nvme {
    struct drv_dev *dev;
    uint32_t stride;                    /* doorbell stride, bytes */
    uint64_t timeout_ms;
    struct queue admin, io;
    uint8_t *page;                      /* a DMA page for identify and block 0 */
    uint64_t blocks;
    uint32_t block_size, max_blocks;
    uint32_t vectors;                   /* MSI-X vectors in use; 0 if polled */
    unsigned busy;                      /* slots in use */
    struct slot slot[IO_DEPTH];
    struct inflight msg[BACKLOG];
    uint16_t order[BACKLOG];            /* messages with requests to submit, oldest first */
    unsigned norder;
    char model[41];
};

static struct nvme ctrls[DRV_DEVICES];
static unsigned nctrls;

static uint32_t rd(const struct nvme *c, uint32_t off)
{
    return *(volatile uint32_t *)(c->dev->regs + off);
}

static uint64_t rd64(const struct nvme *c, uint32_t off)
{
    return rd(c, off) | (uint64_t)rd(c, off + 4) << 32;
}

static void wr(const struct nvme *c, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(c->dev->regs + off) = v;
}

static void wr64(const struct nvme *c, uint32_t off, uint64_t v)
{
    wr(c, off, (uint32_t)v);
    wr(c, off + 4, (uint32_t)(v >> 32));
}

static bool wait_ready(const struct nvme *c, bool ready)
{
    uint64_t until = CurrentTime() + c->timeout_ms * MS;

    while ((rd(c, CSTS) & CSTS_RDY) != (ready ? CSTS_RDY : 0u))
        if (rd(c, CSTS) == ~0u || CurrentTime() > until)
            return false;
    return true;
}

static void *dma(struct drv *d, uint64_t size)
{
    void *at;

    return AllocDmaArea(drv_lease(d), size, 0, &at) < 0 ? nullptr : at;
}

static bool queue_new(struct drv *d, struct queue *q, uint32_t qid, uint32_t depth)
{
    *q = (struct queue){ .depth = depth, .qid = qid, .phase = 1 };
    q->sq = dma(d, PAGE);
    q->cq = dma(d, PAGE);
    return q->sq && q->cq;
}

static void sq_put(struct queue *q, const struct sqe *e)
{
    q->sq[q->tail] = *e;
    q->tail = (q->tail + 1) % q->depth;
}

static void sq_ring(struct nvme *c, struct queue *q)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(c, DOORBELLS + 2 * q->qid * c->stride, q->tail);
}

/* The next completion on 'q', or nullptr; cq_done when it's been read. */
static struct cqe *cq_next(struct queue *q)
{
    struct cqe *e = &q->cq[q->head];

    if ((e->status & 1) != q->phase)
        return nullptr;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return e;
}

static void cq_done(struct queue *q)
{
    if (++q->head == q->depth) {
        q->head = 0;
        q->phase ^= 1;
    }
}

static void cq_ring(struct nvme *c, struct queue *q)
{
    wr(c, DOORBELLS + (2 * q->qid + 1) * c->stride, q->head);
}

/* An admin command, polled (bind's): its status, 0 for success, or -1
 * if none came in time. */
static int admin(struct nvme *c, uint8_t op, uint32_t nsid, uint64_t prp, uint32_t cdw10,
                 uint32_t cdw11)
{
    struct queue *q = &c->admin;
    uint64_t until = CurrentTime() + c->timeout_ms * MS;
    uint16_t cid = ++q->cid;
    struct sqe e = { .cdw0 = op | (uint32_t)cid << 16, .nsid = nsid, .prp1 = prp,
                     .cdw10 = cdw10, .cdw11 = cdw11 };
    struct cqe *done;
    int status;

    sq_put(q, &e);
    sq_ring(c, q);
    while (!(done = cq_next(q)))
        if (CurrentTime() > until)
            return -1;
    status = done->cid == cid ? done->status >> 1 : -2;
    cq_done(q);
    cq_ring(c, q);
    return status;
}

/* --- I/O. --- */

static void finish(struct drv *d, struct nvme *c, unsigned i)
{
    struct inflight *f = &c->msg[i];

    if (--f->left)
        return;
    f->m->bm_Msg.mn_Error = 0;
    drv_reply(d, f->client, &f->m->bm_Msg);
    f->used = false;
}

/* Request r of message i as a command in a free slot; false if it was
 * refused here (its error set) and needs no slot. */
static bool submit(struct nvme *c, unsigned i, unsigned r, unsigned s)
{
    struct inflight *f = &c->msg[i];
    struct BlkReq req = f->m->bm_Req[r];        /* the client's: read once */
    uint64_t bytes = (uint64_t)req.br_Count * c->block_size, page = req.br_Buffer & ~(PAGE - 1);
    uint64_t pages = (req.br_Buffer - page + bytes + PAGE - 1) / PAGE;
    struct slot *sl = &c->slot[s];
    struct sqe e = { .nsid = 1 };

    if (req.br_Op == BLK_FLUSH) {
        e.cdw0 = IO_FLUSH;
    } else if (req.br_Op != BLK_READ && req.br_Op != BLK_WRITE) {
        f->m->bm_Req[r].br_Error = (int32_t)ERR_BADARG;
        return false;
    } else if (req.br_Count == 0 || req.br_Count > c->max_blocks || req.br_Block >= c->blocks ||
               req.br_Count > c->blocks - req.br_Block || req.br_Buffer & 3 ||
               !drv_in_heap(nullptr, f->client, req.br_Buffer, bytes)) {
        f->m->bm_Req[r].br_Error = (int32_t)ERR_BADARG;
        return false;
    } else {
        e.cdw0 = req.br_Op == BLK_READ ? IO_READ : IO_WRITE;
        e.prp1 = req.br_Buffer;
        if (pages == 2)
            e.prp2 = page + PAGE;
        if (pages > 2) {
            for (uint64_t k = 1; k < pages; k++)
                sl->prp[k - 1] = page + k * PAGE;
            e.prp2 = (uint64_t)sl->prp;
        }
        e.cdw10 = (uint32_t)req.br_Block;
        e.cdw11 = (uint32_t)(req.br_Block >> 32);
        e.cdw12 = req.br_Count - 1;
    }
    e.cdw0 |= s << 16;                  /* the command ID is the slot */
    sl->busy = true;
    sl->msg = (uint16_t)i;
    sl->req = (uint16_t)r;
    sq_put(&c->io, &e);
    c->busy++;
    return true;
}

/* Fill free slots from the oldest messages waiting. */
static void submit_waiting(struct drv *d, struct nvme *c)
{
    bool rang = false;
    unsigned s = 0;

    while (c->norder && c->busy < IO_DEPTH - 1) {
        unsigned i = c->order[0];
        struct inflight *f = &c->msg[i];

        while (c->slot[s].busy)
            s++;
        if (submit(c, i, f->next, s))
            rang = true;
        else
            finish(d, c, i);
        if (++f->next == f->count) {
            for (unsigned k = 1; k < c->norder; k++)
                c->order[k - 1] = c->order[k];
            c->norder--;
        }
    }
    if (rang)
        sq_ring(c, &c->io);
}

static void reap(struct drv *d, struct nvme *c)
{
    struct cqe *e;
    bool any = false;

    while ((e = cq_next(&c->io))) {
        uint16_t s = e->cid, status = e->status >> 1;

        cq_done(&c->io);
        any = true;
        if (s >= IO_DEPTH || !c->slot[s].busy) {
            drv_log(d, c->dev, "a completion for no command: ignored");
            continue;
        }
        c->slot[s].busy = false;
        c->busy--;
        c->msg[c->slot[s].msg].m->bm_Req[c->slot[s].req].br_Error = status ? BLK_EIO : 0;
        finish(d, c, c->slot[s].msg);
    }
    if (any)
        cq_ring(c, &c->io);
    submit_waiting(d, c);
}

static void message(struct drv *d, struct drv_dev *dev, unsigned client, struct Message *m)
{
    struct nvme *c = &ctrls[dev - d->dev];
    struct BlkMsg *bm = (struct BlkMsg *)m;
    uint32_t version = bm->bm_Version, op = bm->bm_Op, count = bm->bm_Count;
    unsigned i = 0;

    if (version != BLK_VERSION || (op != BLK_INFO && op != BLK_IO) ||
        (op == BLK_IO && (count == 0 || count > BLK_BATCH))) {
        m->mn_Error = (int32_t)ERR_BADARG;
        drv_reply(d, client, m);
        return;
    }
    if (op == BLK_INFO) {
        bm->bm_Blocks = c->blocks;
        bm->bm_BlockSize = c->block_size;
        bm->bm_MaxBlocks = c->max_blocks;
        bm->bm_ReadOnly = 0;
        m->mn_Error = 0;
        drv_reply(d, client, m);
        return;
    }
    while (i < BACKLOG && c->msg[i].used)
        i++;
    if (i == BACKLOG) {
        m->mn_Error = (int32_t)ERR_FULL;    /* more outstanding than a client may have */
        drv_reply(d, client, m);
        return;
    }
    c->msg[i] = (struct inflight){ .m = bm, .client = client, .count = count, .left = count,
                                   .used = true };
    c->order[c->norder++] = (uint16_t)i;
    submit_waiting(d, c);
}

static void interrupt(struct drv *d, struct drv_dev *dev)
{
    reap(d, &ctrls[dev - d->dev]);
}

static bool poll(struct drv *d)
{
    bool busy = false;

    for (unsigned k = 0; k < nctrls; k++)
        if (!ctrls[k].vectors && ctrls[k].dev && ctrls[k].busy) {
            reap(d, &ctrls[k]);
            busy |= ctrls[k].busy != 0;
        }
    return busy;
}

/* --- Bind. --- */

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

/* MSI-X: each vector writes event i to the doorbell; unmasked. */
static void msix(struct nvme *c, const struct DrvDevice *info)
{
    volatile uint32_t *t = (volatile uint32_t *)info->dd_MsixTable;

    for (uint32_t i = 0; i < c->vectors; i++) {
        t[4 * i] = (uint32_t)info->dd_MsiDoorbell;
        t[4 * i + 1] = (uint32_t)(info->dd_MsiDoorbell >> 32);
        t[4 * i + 2] = i;
        t[4 * i + 3] = 0;
    }
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    struct nvme *c = &ctrls[dev - d->dev];
    const struct DrvDevice *info = dev->info;
    uint64_t cap;
    uint32_t flbas, lbads, mdts, iv;
    uint64_t *prps;
    unsigned n = 0;
    char line[160];
    struct sqe e;
    struct cqe *done;

    *c = (struct nvme){ .dev = dev };
    if (dev - d->dev >= nctrls)
        nctrls = (unsigned)(dev - d->dev) + 1;
    if (info->dd_Size < 0x2000)
        return DRV_DECLINE;
    cap = rd64(c, CAP);
    if (cap == ~0ULL || !(cap >> 37 & 1))      /* CSS: the NVM command set */
        return DRV_DECLINE;
    c->stride = 4u << (cap >> 32 & 0xf);
    c->timeout_ms = 500 * ((cap >> 24 & 0xff) + 1);
    if (DOORBELLS + 4 * c->stride > info->dd_Size || (cap >> 48 & 0xf) > 0 ||
        (cap & 0xffff) + 1 < IO_DEPTH)
        return DRV_DECLINE;                     /* 4 KiB pages, and queues deep enough */
    if (info->dd_NLines && info->dd_MsixTable &&
        info->dd_MsixTable >= info->dd_Base &&
        info->dd_MsixTable - info->dd_Base + 16 * info->dd_NLines <= info->dd_Size)
        c->vectors = info->dd_NLines > 2 ? 2 : info->dd_NLines;
    iv = c->vectors ? c->vectors - 1 : 0;

    /* Reset it, then the admin queue (§3.5.1). */
    wr(c, CC, 0);
    if (!wait_ready(c, false))
        return -10;
    if (!queue_new(d, &c->admin, 0, ADMIN_DEPTH) || !queue_new(d, &c->io, 1, IO_DEPTH) ||
        !(c->page = dma(d, PAGE)) || !(prps = dma(d, IO_DEPTH * PRP_ENTRIES * 8)))
        return -11;
    for (unsigned s = 0; s < IO_DEPTH; s++)
        c->slot[s].prp = prps + s * PRP_ENTRIES;
    if (c->vectors)
        msix(c, info);
    wr(c, INTMS, ~0u);                          /* pin interrupts: MSI-X or polling */
    wr(c, AQA, (ADMIN_DEPTH - 1) << 16 | (ADMIN_DEPTH - 1));
    wr64(c, ASQ, (uint64_t)c->admin.sq);
    wr64(c, ACQ, (uint64_t)c->admin.cq);
    wr(c, CC, 4u << 20 | 6u << 16 | CC_EN);     /* 16-byte CQEs, 64-byte SQEs, 4 KiB pages */
    if (!wait_ready(c, true) || rd(c, CSTS) & CSTS_CFS)
        return -12;

    /* Who it is, and namespace 1. */
    if (admin(c, ADM_IDENTIFY, 0, (uint64_t)c->page, 1, 0) != 0)
        return -13;
    for (unsigned i = 0; i < 40; i++)
        c->model[i] = (char)c->page[24 + i];
    for (unsigned i = 40; i > 0 && (c->model[i - 1] == ' ' || c->model[i - 1] == 0); i--)
        c->model[i - 1] = '\0';
    mdts = c->page[77];
    if (admin(c, ADM_IDENTIFY, 1, (uint64_t)c->page, 0, 0) != 0)
        return -14;
    for (unsigned i = 0; i < 8; i++)
        c->blocks |= (uint64_t)c->page[i] << 8 * i;
    flbas = c->page[26] & 0xf;
    lbads = c->page[128 + 4 * flbas + 2];
    if (lbads < 9 || lbads > 12 || c->blocks == 0) {
        drv_log(d, dev, "namespace 1 has blocks this driver can't read yet");
        return DRV_DECLINE;
    }
    c->block_size = 1u << lbads;
    c->max_blocks = MAX_BYTES >> lbads;
    if (mdts && mdts < 6 && (PAGE << mdts) >> lbads < c->max_blocks)
        c->max_blocks = (uint32_t)((PAGE << mdts) >> lbads);

    /* One I/O queue pair (§5.4, §5.5), interrupting by the last vector if any. */
    if (admin(c, ADM_SET_FEATURES, 0, 0, 0x07, 0) != 0 ||
        admin(c, ADM_CREATE_CQ, 0, (uint64_t)c->io.cq, (IO_DEPTH - 1) << 16 | 1,
              iv << 16 | (c->vectors ? 2 : 0) | 1) != 0 ||
        admin(c, ADM_CREATE_SQ, 0, (uint64_t)c->io.sq, (IO_DEPTH - 1) << 16 | 1, 1u << 16 | 1) != 0)
        return -15;

    /* Block 0, polled, to show the path works end to end. */
    e = (struct sqe){ .cdw0 = IO_READ | (IO_DEPTH - 1) << 16, .nsid = 1, .prp1 = (uint64_t)c->page };
    sq_put(&c->io, &e);
    sq_ring(c, &c->io);
    for (uint64_t until = CurrentTime() + c->timeout_ms * MS; !(done = cq_next(&c->io));)
        if (CurrentTime() > until)
            return -16;
    if (done->status >> 1)
        return -17;
    cq_done(&c->io);
    cq_ring(c, &c->io);

    put(line, &n, c->model);
    put(line, &n, ", ");
    num(line, &n, c->blocks);
    put(line, &n, " blocks of ");
    num(line, &n, c->block_size);
    put(line, &n, c->page[510] == 0x55 && c->page[511] == 0xaa ? "; block 0 has a boot signature"
                                                                : "; block 0 has no boot signature");
    put(line, &n, c->vectors ? "; MSI-X" : "; polled");
    line[n] = '\0';
    drv_log(d, dev, line);
    return DRV_OK;
}

static int start(struct drv *d, struct drv_dev *dev)
{
    return (int)drv_publish(d, dev, "nvme");
}

static const struct drv_ops ops = {
    .name = "nvme", .bind = bind, .start = start, .interrupt = interrupt, .message = message,
    .message_size = sizeof(struct BlkMsg), .poll = poll,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
