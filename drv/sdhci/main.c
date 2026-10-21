/*
 * drv/sdhci/main.c -- SD host controllers (SD Host Controller Simplified
 * Specification 3.00) and the SD cards in them (SD Physical Layer
 * Simplified Specification 6.00)
 *
 * The K1's SD slot by its device tree (V4 step 5), and any SDHCI-class
 * PCI function (080501), which is how QEMU's sdhci-pci shows the same
 * code on QEMU.
 *
 * Bind resets the controller, then takes the card through
 * identification at 400 kHz (CMD0, CMD8, ACMD41, CMD2, CMD3), reads its
 * size from the CSD, selects it, and runs it 4 bits wide at 25 MHz. It
 * takes SD (2.0 and later) cards only: SDSC, SDHC and SDXC, not MMC.
 *
 * It serves the block class (planar/devices/block.h) on "sd.N", one
 * command at a time, since a card does one at a time: each request is a
 * CMD18 or CMD25 with Auto CMD12, its data moved by ADMA2 straight to or
 * from the client's buffer, or through a bounce buffer when that lies
 * above 4 GiB, where 32-bit ADMA2 can't reach (the RV2's 6 GiB there).
 * Requests wait their turn here. Completion is by its interrupt line
 * when it has one, else it polls.
 */
#include <stddef.h>

#include <planar/devices/block.h>
#include <planar/drv.h>
#include <planar/syscalls.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr uint64_t MS = 1000000;
constexpr uint64_t PAGE = 4096;
constexpr uint32_t BLOCK = 512;
constexpr uint32_t MAX_BLOCKS = 256;            /* 128 KiB a request: two ADMA2 lines */
constexpr unsigned BACKLOG = 64;

enum : uint32_t {
    SDMA = 0x00, BLKSIZE = 0x04, ARG = 0x08, XFER = 0x0c, RESP = 0x10, PRESENT = 0x24,
    HOSTCTL = 0x28, CLOCK = 0x2c, INTSTS = 0x30, INTEN = 0x34, SIGEN = 0x38, HOSTCTL2 = 0x3c,
    CAPS = 0x40, ADMA_ERR = 0x54, ADMA_ADDR = 0x58, VERSION = 0xfc,
};

/* XFER's low half (transfer mode), and high half (command). */
enum : uint32_t {
    XM_DMA = 1u << 0, XM_COUNT = 1u << 1, XM_ACMD12 = 1u << 2, XM_READ = 1u << 4,
    XM_MULTI = 1u << 5,
};
enum : uint32_t {
    R_NONE = 0, R_136 = 1, R_48 = 2, R_48B = 3, C_CRC = 1u << 3, C_INDEX = 1u << 4,
    C_DATA = 1u << 5,
};
enum : uint32_t { I_CMD = 1u << 0, I_XFER = 1u << 1, I_ERR = 1u << 15 };
enum : uint32_t { P_CMD = 1u << 0, P_DAT = 1u << 1, P_CARD = 1u << 16 };

/* What a command expects back. */
enum rsp : uint8_t { NONE, R1, R1B, R2, R3, R6, R7 };

struct adma {                           /* ADMA2, 32-bit: one line */
    uint16_t attr;                      /* valid, end, act = transfer */
    uint16_t len;                       /* bytes; 0 is 65536 */
    uint32_t addr;
};

struct waiting {
    struct BlkMsg *m;
    unsigned client, next, count;
    bool used;
};

struct sd {
    struct drv_dev *dev;
    bool k1;                            /* the K1's: its quirks (k1_reset) */
    struct adma *table;                 /* in a DMA Area below 4 GiB */
    uint8_t *bounce;                    /* likewise, for buffers above */
    uint32_t base_hz;
    uint32_t rca;
    bool sdhc;                          /* block addressed */
    uint64_t blocks;
    /* The request on the card now, if any. */
    bool busy;
    unsigned cur, req;
    uint8_t *bounced;                   /* the client's buffer, if through the bounce one */
    uint64_t bytes;
    uint64_t deadline;
    /* Messages, oldest first. */
    struct waiting msg[BACKLOG];
    uint16_t order[BACKLOG];
    unsigned norder;
};

static struct sd sds[DRV_DEVICES];
static unsigned nsds;

static uint32_t rd(const struct sd *s, uint32_t off)
{
    return *(volatile uint32_t *)(s->dev->regs + off);
}

static uint16_t rd16(const struct sd *s, uint32_t off)
{
    return *(volatile uint16_t *)(s->dev->regs + off);
}

static uint8_t rd8(const struct sd *s, uint32_t off)
{
    return *(volatile uint8_t *)(s->dev->regs + off);
}

static void wr(const struct sd *s, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(s->dev->regs + off) = v;
}

static void wr16(const struct sd *s, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(s->dev->regs + off) = v;
}

static void wr8(const struct sd *s, uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)(s->dev->regs + off) = v;
}

static void nap_us(uint64_t us)
{
    uint64_t until = CurrentTime() + us * 1000;

    while (CurrentTime() < until)
        ;
}

/* --- The controller. --- */

/*
 * The K1 (SpacemiT's SDHCI, as Linux's sdhci-of-k1 drives it): after a
 * full reset its PHY and pads are set up again, and its card clock
 * forced on, as for an SD-only slot; its card clock is the internal one
 * at SD speeds. It also has no 64-bit DMA, a base clock its capabilities
 * get wrong, and card detection by GPIO, not the controller.
 */
enum : uint32_t {
    K1_OP_EXT = 0x108, K1_LEGACY = 0x10c, K1_TX_CFG = 0x11c, K1_PHY_CTRL = 0x160,
    K1_PHY_PADCFG = 0x178,
};
constexpr uint32_t K1_BASE_HZ = 204800000;      /* its device tree's clk-src-freq */

static void k1_reset(struct sd *s)
{
    wr(s, K1_PHY_CTRL, rd(s, K1_PHY_CTRL) | 1u << 0 | 1u << 1);    /* PHY on, PLL lock */
    wr(s, K1_PHY_PADCFG, (rd(s, K1_PHY_PADCFG) & ~7u) | 4 | 1u << 5);   /* drive 4, RX bias */
    wr(s, K1_LEGACY, rd(s, K1_LEGACY) | 1u << 6);                  /* pad clock on */
    wr(s, K1_OP_EXT, rd(s, K1_OP_EXT) | 1u << 11 | 1u << 12);      /* card clock forced on */
    wr(s, K1_TX_CFG, rd(s, K1_TX_CFG) | 1u << 30);                 /* internal TX clock */
}

static bool reset(struct sd *s, uint8_t what)
{
    uint64_t until = CurrentTime() + 100 * MS;

    wr8(s, CLOCK + 3, what);
    while (rd8(s, CLOCK + 3) & what)
        if (CurrentTime() > until)
            return false;
    if (s->k1 && what & 1)
        k1_reset(s);
    return true;
}

/* The card's clock at 'hz' or the nearest below (3.00: a 10-bit divisor
 * of 2N, N = 0 the base itself). */
static bool clock(struct sd *s, uint32_t hz)
{
    uint32_t n = 0;
    uint64_t until = CurrentTime() + 100 * MS;

    wr16(s, CLOCK, 0);
    if (s->base_hz > hz)
        for (n = 1; n < 1023 && s->base_hz / (2 * n) > hz; n++)
            ;
    wr16(s, CLOCK, (uint16_t)((n & 0xff) << 8 | (n >> 8 & 3) << 6 | 1));
    while (!(rd16(s, CLOCK) & 2))               /* internal clock stable */
        if (CurrentTime() > until)
            return false;
    wr16(s, CLOCK, rd16(s, CLOCK) | 4);         /* to the card */
    nap_us(1000);
    return true;
}

/* A command with no data, polled: 0, or the error status (or 1 for a
 * timeout here). */
static int command(struct sd *s, uint32_t idx, uint32_t arg, enum rsp r)
{
    static const uint16_t flags[] = {
        [NONE] = R_NONE, [R1] = R_48 | C_CRC | C_INDEX, [R1B] = R_48B | C_CRC | C_INDEX,
        [R2] = R_136 | C_CRC, [R3] = R_48, [R6] = R_48 | C_CRC | C_INDEX,
        [R7] = R_48 | C_CRC | C_INDEX,
    };
    uint64_t until = CurrentTime() + 500 * MS;
    uint32_t st;

    while (rd(s, PRESENT) & (P_CMD | (r == R1B ? P_DAT : 0)))
        if (CurrentTime() > until)
            return 1;
    wr(s, INTSTS, ~0u);
    wr(s, ARG, arg);
    wr(s, XFER, (idx << 8 | flags[r]) << 16);
    while (!((st = rd(s, INTSTS)) & (I_CMD | I_ERR)))
        if (CurrentTime() > until)
            return 1;
    if (st & I_ERR) {
        wr(s, INTSTS, st);
        reset(s, 2);                            /* the command line */
        return (int)(st >> 16);
    }
    wr(s, INTSTS, I_CMD);
    if (r == R1B) {
        while (!((st = rd(s, INTSTS)) & (I_XFER | I_ERR)))
            if (CurrentTime() > until)
                return 1;
        wr(s, INTSTS, st);
    }
    return 0;
}

static int app_command(struct sd *s, uint32_t idx, uint32_t arg, enum rsp r)
{
    int e = command(s, 55, s->rca << 16, R1);

    return e ? e : command(s, idx, arg, r);
}

/* --- Requests. --- */

static void finish(struct drv *d, struct sd *s, unsigned i)
{
    struct waiting *w = &s->msg[i];

    w->m->bm_Msg.mn_Error = 0;
    drv_reply(d, w->client, &w->m->bm_Msg);
    w->used = false;
    for (unsigned k = 1; k < s->norder; k++)
        s->order[k - 1] = s->order[k];
    s->norder--;
}

/* Start the next request on the card, if it's free and one waits. */
static void next(struct drv *d, struct sd *s)
{
    while (!s->busy && s->norder) {
        unsigned i = s->order[0];
        struct waiting *w = &s->msg[i];
        struct BlkReq r;
        uint64_t bytes;
        bool read;

        if (w->next == w->count) {
            finish(d, s, i);
            continue;
        }
        r = w->m->bm_Req[w->next];              /* the client's: read once */
        bytes = (uint64_t)r.br_Count * BLOCK;
        read = r.br_Op == BLK_READ;
        if (r.br_Op == BLK_FLUSH) {
            w->m->bm_Req[w->next++].br_Error = 0;   /* nothing cached here */
            continue;
        }
        if ((r.br_Op != BLK_READ && r.br_Op != BLK_WRITE) || r.br_Count == 0 ||
            r.br_Count > MAX_BLOCKS || r.br_Block >= s->blocks ||
            r.br_Count > s->blocks - r.br_Block || r.br_Buffer & 3 ||
            !drv_in_heap(d, w->client, r.br_Buffer, bytes)) {
            w->m->bm_Req[w->next++].br_Error = (int32_t)ERR_BADARG;
            continue;
        }
        /* ADMA2's addresses are 32-bit: a buffer above goes by the bounce one. */
        s->bounced = nullptr;
        s->bytes = bytes;
        if (r.br_Buffer + bytes > 1ULL << 32) {
            s->bounced = (uint8_t *)r.br_Buffer;
            if (!read)
                memcpy(s->bounce, s->bounced, bytes);
            r.br_Buffer = (uint64_t)s->bounce;
        }
        /* ADMA2: 64 KiB a line. */
        for (unsigned k = 0; k * 65536 < bytes; k++) {
            uint64_t len = bytes - k * 65536ULL > 65536 ? 65536 : bytes - k * 65536ULL;

            s->table[k] = (struct adma){
                .attr = (uint16_t)(1u | 2u << 4 | ((k + 1) * 65536ULL >= bytes ? 2u : 0)),
                .len = (uint16_t)len, .addr = (uint32_t)(r.br_Buffer + k * 65536ULL),
            };
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        wr(s, INTSTS, ~0u);
        wr(s, ADMA_ADDR, (uint32_t)(uint64_t)s->table);
        wr(s, ADMA_ADDR + 4, 0);
        wr(s, BLKSIZE, r.br_Count << 16 | BLOCK);
        wr(s, ARG, (uint32_t)(s->sdhc ? r.br_Block : r.br_Block * BLOCK));
        wr(s, XFER, ((read ? 18u : 25u) << 8 | R_48 | C_CRC | C_INDEX | C_DATA) << 16 |
                        XM_DMA | XM_COUNT | XM_ACMD12 | XM_MULTI | (read ? XM_READ : 0));
        s->busy = true;
        s->cur = i;
        s->req = w->next;
        s->deadline = CurrentTime() + 2000 * MS;
    }
}

/* The request on the card: done, failed, or still going. */
static void check(struct drv *d, struct sd *s)
{
    uint32_t st;
    struct waiting *w;

    if (!s->busy)
        return;
    st = rd(s, INTSTS);
    w = &s->msg[s->cur];
    if (st & I_ERR || (!(st & I_XFER) && CurrentTime() > s->deadline)) {
        wr(s, INTSTS, st);
        reset(s, 2 | 4);                        /* the command and data lines */
        w->m->bm_Req[s->req].br_Error = BLK_EIO;
    } else if (st & I_XFER) {
        wr(s, INTSTS, st);
        if (s->bounced && w->m->bm_Req[s->req].br_Op == BLK_READ)
            memcpy(s->bounced, s->bounce, s->bytes);
        w->m->bm_Req[s->req].br_Error = 0;
    } else {
        return;
    }
    w->next++;
    s->busy = false;
    next(d, s);
}

static void message(struct drv *d, struct drv_dev *dev, unsigned client, struct Message *m)
{
    struct sd *s = &sds[dev - d->dev];
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
        bm->bm_Blocks = s->blocks;
        bm->bm_BlockSize = BLOCK;
        bm->bm_MaxBlocks = MAX_BLOCKS;
        bm->bm_ReadOnly = 0;
        m->mn_Error = 0;
        drv_reply(d, client, m);
        return;
    }
    while (i < BACKLOG && s->msg[i].used)
        i++;
    if (i == BACKLOG) {
        m->mn_Error = (int32_t)ERR_FULL;
        drv_reply(d, client, m);
        return;
    }
    s->msg[i] = (struct waiting){ .m = bm, .client = client, .count = count, .used = true };
    s->order[s->norder++] = (uint16_t)i;
    next(d, s);
}

static void interrupt(struct drv *d, struct drv_dev *dev)
{
    check(d, &sds[dev - d->dev]);
}

static bool poll(struct drv *d)
{
    bool busy = false;

    for (unsigned k = 0; k < nsds; k++)
        if (sds[k].dev && !sds[k].dev->line_bound && sds[k].busy) {
            check(d, &sds[k]);
            busy |= sds[k].busy;
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

static int bind(struct drv *d, struct drv_dev *dev)
{
    struct sd *s = &sds[dev - d->dev];
    uint64_t caps, until;
    uint32_t ocr = 0, csd[4];
    unsigned n = 0;
    char line[160];
    void *at;
    int e;

    *s = (struct sd){ .dev = dev };
    for (const char *k = dev->info->dd_Key; *k; k++)
        if (k[0] == 'x' && k[1] == '1' && k[2] == '-')
            s->k1 = true;           /* "ky,x1-sdhci" */
        else if (k[0] == 'k' && k[1] == '1' && k[2] == '-')
            s->k1 = true;           /* "spacemit,k1-sdhci" */
    if (dev - d->dev >= nsds)
        nsds = (unsigned)(dev - d->dev) + 1;
    if (dev->info->dd_Size < 0x100 || rd(s, CAPS) == ~0u)
        return DRV_DECLINE;
    caps = rd(s, CAPS) | (uint64_t)rd(s, CAPS + 4) << 32;
    if (!(caps & 1u << 19)) {
        drv_log(d, dev, "no ADMA2: not driven");
        return DRV_DECLINE;
    }
    s->base_hz = (uint32_t)(caps >> 8 & ((rd16(s, VERSION) & 0xff) >= 2 ? 0xff : 0x3f)) * 1000000;
    if (s->k1 || !s->base_hz)
        s->base_hz = K1_BASE_HZ;    /* the K1's capabilities are wrong; else unknown */
    if (AllocDmaArea(drv_lease(d), PAGE, AREAF_DMA32, &at) < 0)
        return -10;
    s->table = at;
    if (AllocDmaArea(drv_lease(d), MAX_BLOCKS * BLOCK, AREAF_DMA32, &at) < 0)
        return -11;
    s->bounce = at;

    if (!reset(s, 1))
        return -12;
    if (!(rd(s, PRESENT) & P_CARD)) {
        if (!s->k1) {
            drv_log(d, dev, "no card");
            return DRV_DECLINE;
        }
        /* Card detection is a GPIO's: say there's one, and find out. */
        wr8(s, HOSTCTL, 3 << 6);
    }
    wr8(s, HOSTCTL + 1, 7 << 1 | 1);            /* 3.3 V, on */
    wr8(s, HOSTCTL, (rd8(s, HOSTCTL) & 3 << 6) | 2 << 3);  /* ADMA2, 32-bit, 1 bit wide */
    wr8(s, CLOCK + 2, 0xe);                     /* the longest data timeout */
    wr(s, INTEN, ~0u);
    wr(s, SIGEN, dev->info->dd_Line != DRV_NOLINE ? (I_XFER | I_ERR | 0xffffu << 16) : 0);
    if (!clock(s, 400000))
        return -13;

    /* Identification (§4.2). */
    command(s, 0, 0, NONE);
    if ((e = command(s, 8, 0x1aa, R7)) != 0 || (rd(s, RESP) & 0xfff) != 0x1aa) {
        drv_log(d, dev, e == 1 || e & 1 ? "no card answers" : "not an SD 2.0 card: not driven");
        return DRV_DECLINE;
    }
    for (until = CurrentTime() + 1000 * MS; !(ocr & 1u << 31);) {
        if (app_command(s, 41, 0x40ff8000, R3) != 0 || CurrentTime() > until)
            return -14;
        ocr = rd(s, RESP);
    }
    s->sdhc = ocr & 1u << 30;
    if (command(s, 2, 0, R2) != 0 || command(s, 3, 0, R6) != 0)
        return -15;
    s->rca = rd(s, RESP) >> 16;
    if (command(s, 9, s->rca << 16, R2) != 0)
        return -16;
    for (unsigned k = 0; k < 4; k++)
        csd[k] = rd(s, RESP + 4 * k);           /* bits 127:8 of it, from bit 0 */
    if ((csd[3] >> 22 & 3) == 1) {              /* CSD 2.0: C_SIZE, bits 69:48 */
        s->blocks = ((uint64_t)((csd[1] >> 8 & 0xffffff) & 0x3fffff) + 1) * 1024;
    } else {                                    /* CSD 1.0 */
        uint32_t c_size = (csd[2] & 0x3) << 10 | csd[1] >> 22;
        uint32_t mult = csd[1] >> 7 & 7, bl = csd[2] >> 8 & 0xf;

        s->blocks = ((uint64_t)(c_size + 1) << (mult + 2)) << bl >> 9;
    }
    if (command(s, 7, s->rca << 16, R1B) != 0 || app_command(s, 6, 2, R1) != 0 ||
        (!s->sdhc && command(s, 16, BLOCK, R1) != 0))
        return -17;
    wr8(s, HOSTCTL, rd8(s, HOSTCTL) | 2);       /* 4 bits wide */
    if (!clock(s, 25000000))
        return -18;

    put(line, &n, s->sdhc ? "SDHC/SDXC card, " : "SDSC card, ");
    num(line, &n, s->blocks);
    put(line, &n, " blocks of 512 at ");
    num(line, &n, s->base_hz / 1000000);
    put(line, &n, " MHz base; ");
    put(line, &n, dev->info->dd_Line != DRV_NOLINE ? "by interrupt" : "polled");
    line[n] = '\0';
    drv_log(d, dev, line);
    return DRV_OK;
}

static int start(struct drv *d, struct drv_dev *dev)
{
    return (int)drv_publish(d, dev, "sd");
}

static const struct drv_ops ops = {
    .name = "sdhci", .bind = bind, .start = start, .interrupt = interrupt, .message = message,
    .message_size = sizeof(struct BlkMsg), .poll = poll,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
