/*
 * lib/blk/blk.c -- the block class's client library (planar/blk.h)
 */
#include <stddef.h>

#include <planar/blk.h>
#include <planar/syscalls.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr uint64_t PAGE = 4096;
constexpr uint64_t MS = 1000000;
constexpr uint32_t GPT_ENTRY_MIN = 128;

/* --- Connecting. --- */

/* DoMsg with the class's deadline; a driver that misses it is reported. */
static int64_t ask(struct blk *b, struct BlkMsg *m)
{
    int64_t e = DoMsgUntil(&b->mp, &m->bm_Msg, CurrentTime() + BLK_IO_MS * MS);

    if (e == ERR_TIMEOUT)
        ReportUnresponsive(b->port);
    return e;
}

static int64_t info(struct blk *b)
{
    struct BlkMsg *m = b->msg[0];
    int64_t e;

    memset(m, 0, sizeof *m);
    m->bm_Msg.mn_Length = sizeof *m;
    m->bm_Version = BLK_VERSION;
    m->bm_Op = BLK_INFO;
    if ((e = ask(b, m)) != 0 || (e = m->bm_Msg.mn_Error) != 0)
        return e;
    /* The driver's answer: checked before it's believed (§11.9). */
    if (m->bm_BlockSize < 512 || m->bm_BlockSize > 65536 ||
        (m->bm_BlockSize & (m->bm_BlockSize - 1)) || m->bm_MaxBlocks == 0 || m->bm_Blocks == 0)
        return ERR_BADARG;
    if (b->gen > 1 && (m->bm_BlockSize != b->block_size || m->bm_Blocks != b->dev_blocks))
        return ERR_BADARG;              /* not the device it was */
    b->block_size = m->bm_BlockSize;
    b->dev_blocks = m->bm_Blocks;
    b->max_blocks = m->bm_MaxBlocks;
    b->read_only = m->bm_ReadOnly != 0;
    return 0;
}

/* A connection, its messages and its bounce buffer, and what the device
 * is. A driver restarting may not have published its port yet (OpenPort
 * waits for it), or may die again as this asks: both are waited out,
 * for up to BLK_GONE_MS. ERR_NOTFOUND if no port came by then. */
static int64_t connect_for(struct blk *b, uint64_t wait_ms)
{
    uint64_t until = wait_ms ? CurrentTime() + wait_ms * MS : 0;
    int64_t e;

    for (;;) {
        e = ConnectPortUntil(&b->mp, b->port, b->sigbit, 2 * BLK_MSGS, b->heap, until);
        if (e == ERR_TIMEOUT)
            e = ERR_NOTFOUND;
        if (e == 0) {
            b->open = true;
            b->gen++;
            b->free = 0;
            for (unsigned i = 0; i < BLK_MSGS && e == 0; i++) {
                if (!(b->msg[i] = AllocMsg(&b->mp, sizeof(struct BlkMsg))))
                    e = ERR_NOMEM;
                b->free |= 1u << i;
            }
            if (e == 0 && (e = info(b)) == 0)
                break;
            ClosePort(&b->mp);
            b->open = false;
        }
        if (e != ERR_PORT_DIED || CurrentTime() >= until)
            return e;               /* (died as it was asked: once more) */
    }
    b->bounce = blk_alloc(b, (uint64_t)b->max_blocks * b->block_size);
    b->scratch = blk_alloc(b, b->block_size);
    return b->bounce && b->scratch ? 0 : ERR_NOMEM;
}

static int64_t connect(struct blk *b)
{
    return connect_for(b, BLK_GONE_MS);
}

static int64_t reconnect(struct blk *b)
{
    if (b->open)
        ClosePort(&b->mp);
    b->open = false;
    b->reconnects++;
    return connect(b);
}

static int64_t open_for(struct blk *b, const char *port, int64_t sigbit, uint64_t heap,
                        uint64_t wait_ms)
{
    int64_t e;
    unsigned i = 0;

    memset(b, 0, sizeof *b);
    for (; port[i] && i < sizeof b->port - 1; i++)
        b->port[i] = port[i];
    b->sigbit = sigbit;
    b->heap = heap;
    if ((e = connect_for(b, wait_ms)) != 0) {
        blk_close(b);
        return e;
    }
    b->first = 0;
    b->blocks = b->dev_blocks;
    return 0;
}

int64_t blk_open(struct blk *b, const char *port, int64_t sigbit, uint64_t heap)
{
    return open_for(b, port, sigbit, heap, BLK_GONE_MS);
}

int64_t blk_open_now(struct blk *b, const char *port, int64_t sigbit, uint64_t heap)
{
    return open_for(b, port, sigbit, heap, 0);
}

void blk_close(struct blk *b)
{
    if (b->open)
        ClosePort(&b->mp);
    b->open = false;
}

void *blk_alloc(struct blk *b, uint64_t size)
{
    uint64_t at = (b->mp.mp_HeapNext + PAGE - 1) & ~(PAGE - 1);

    if (!b->open || size == 0 || at > b->mp.mp_HeapHi || size > b->mp.mp_HeapHi - at)
        return nullptr;
    b->mp.mp_HeapNext = at + size;
    return (void *)at;
}

static bool in_heap(const struct blk *b, const void *p, uint64_t size)
{
    uint64_t a = (uint64_t)p;

    return b->open && a >= b->mp.mp_HeapLo && a <= b->mp.mp_HeapHi && size <= b->mp.mp_HeapHi - a;
}

/* --- Transfers. --- */

static struct BlkMsg *take(struct blk *b)
{
    for (unsigned i = 0; i < BLK_MSGS; i++)
        if (b->free & 1u << i) {
            b->free &= ~(1u << i);
            return b->msg[i];
        }
    return nullptr;
}

void blk_release(struct blk *b, struct BlkMsg *m)
{
    for (unsigned i = 0; i < BLK_MSGS; i++)
        if (b->msg[i] == m)
            b->free |= 1u << i;
}

unsigned blk_out(const struct blk *b)
{
    return BLK_MSGS - (unsigned)__builtin_popcount(b->free);
}

static void fill(struct blk *b, struct BlkMsg *m, uint32_t op, uint64_t block, uint32_t count,
                 const void *buf)
{
    memset(m, 0, offsetof(struct BlkMsg, bm_Req) + sizeof m->bm_Req[0]);
    m->bm_Msg.mn_Length = sizeof *m;
    m->bm_Version = BLK_VERSION;
    m->bm_Op = BLK_IO;
    m->bm_Count = 1;
    m->bm_Req[0] = (struct BlkReq){
        .br_Op = op, .br_Block = b->first + block, .br_Count = count, .br_Buffer = (uint64_t)buf,
    };
}

/* One request, synchronously, on the current connection. */
static int64_t once(struct blk *b, uint32_t op, uint64_t block, uint32_t count, const void *buf)
{
    struct BlkMsg *m = take(b);
    int64_t e;

    if (!m)
        return ERR_FULL;
    fill(b, m, op, block, count, buf);
    e = ask(b, m);
    if (e == ERR_TIMEOUT)
        return e;                   /* the driver's still: not ours to reuse */
    if (e == 0 && (e = m->bm_Msg.mn_Error) == 0)
        e = m->bm_Req[0].br_Error;
    blk_release(b, m);
    return e;
}

static int64_t transfer(struct blk *b, uint32_t op, uint64_t block, uint32_t count, void *buf,
                        bool to_dev)
{
    uint64_t bytes = (uint64_t)count * b->block_size;
    int64_t e = 0;

    if (!b->open || count == 0 || block >= b->blocks || count > b->blocks - block)
        return ERR_BADARG;
    if (in_heap(b, buf, bytes)) {
        if (count > b->max_blocks)
            return ERR_BADARG;
        e = once(b, op, block, count, buf);
        if (e == ERR_PORT_DIED && reconnect(b) == 0)
            return ERR_PORT_DIED;       /* its heap went: the caller's to redo */
        return e;
    }
    /* Bounced, a piece at a time, each retried across a restart. */
    for (uint32_t done = 0; done < count;) {
        uint32_t n = count - done < b->max_blocks ? count - done : b->max_blocks;
        uint8_t *at = (uint8_t *)buf + (uint64_t)done * b->block_size;

        for (uint64_t since = CurrentTime();;) {
            if (to_dev)
                memcpy(b->bounce, at, (uint64_t)n * b->block_size);
            e = once(b, op, block + done, n, b->bounce);
            if (e != ERR_PORT_DIED || CurrentTime() - since > BLK_GONE_MS * MS)
                break;
            if ((e = reconnect(b)) != 0)
                return e;
        }
        if (e != 0)
            return e;
        if (!to_dev)
            memcpy(at, b->bounce, (uint64_t)n * b->block_size);
        done += n;
    }
    return 0;
}

int64_t blk_read(struct blk *b, uint64_t block, uint32_t count, void *buf)
{
    return transfer(b, BLK_READ, block, count, buf, false);
}

int64_t blk_write(struct blk *b, uint64_t block, uint32_t count, const void *buf)
{
    if (b->read_only)
        return ERR_RIGHTS;
    return transfer(b, BLK_WRITE, block, count, (void *)buf, true);
}

struct BlkMsg *blk_submit(struct blk *b, uint64_t block, uint32_t count, void *buf)
{
    struct BlkMsg *m;

    if (!b->open || count == 0 || count > b->max_blocks || block >= b->blocks ||
        count > b->blocks - block || !in_heap(b, buf, (uint64_t)count * b->block_size) ||
        !(m = take(b)))
        return nullptr;
    fill(b, m, BLK_READ, block, count, buf);
    if (PutMsg(&b->mp, &m->bm_Msg) != 0) {
        blk_release(b, m);
        return nullptr;
    }
    return m;
}

/* The next reply, waiting for it; one of ours, or nullptr once the
 * driver has gone (then everything out has failed: blk_release them,
 * and reopen with blk_read or blk_open). */
struct BlkMsg *blk_done(struct blk *b)
{
    struct Message *r;

    while ((r = WaitPort(&b->mp))) {
        for (unsigned i = 0; i < BLK_MSGS; i++)
            if (&b->msg[i]->bm_Msg == r && !(b->free & 1u << i))
                return b->msg[i];
    }
    return nullptr;
}

/* --- GPT. --- */

static uint32_t crc32(const uint8_t *p, uint64_t n, uint32_t crc)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (unsigned k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)p[i] << 8 * i;
    return v;
}

int64_t blk_partition(struct blk *b, unsigned index)
{
    uint64_t first, count;
    int64_t e;

    b->first = 0;
    b->blocks = b->dev_blocks;
    if ((e = blk_partition_range(b, index, &first, &count)) != 0)
        return e;
    b->first = first;
    b->blocks = count;
    return 0;
}

/* No GPT: an MBR's primary partitions, 1 to 4, as SD cards often have.
 * A protective MBR (type EE) without its GPT is no table at all. */
static int64_t mbr_range(struct blk *b, uint8_t *blk, unsigned index, uint64_t *first,
                         uint64_t *count)
{
    const uint8_t *e;
    int64_t err;

    if (index > 4 || (err = blk_read(b, 0, 1, blk)) != 0)
        return index > 4 ? ERR_NOTFOUND : err;
    if (blk[510] != 0x55 || blk[511] != 0xaa)
        return ERR_NOTFOUND;
    e = blk + 446 + 16 * (index - 1);
    if (e[4] == 0 || e[4] == 0xee || le(e + 12, 4) == 0)
        return ERR_NOTFOUND;
    *first = le(e + 8, 4);
    *count = le(e + 12, 4);
    if (*first == 0 || *first >= b->dev_blocks || *count > b->dev_blocks - *first)
        return ERR_BADARG;
    return 0;
}

int64_t blk_partition_range(struct blk *b, unsigned index, uint64_t *first_out, uint64_t *count)
{
    uint8_t *blk = b->scratch;          /* one block at a time, the connection's own */
    uint64_t bs = b->block_size, entries, first = 0, last = 0, was_first = b->first;
    uint64_t was_blocks = b->blocks, tblocks;
    uint32_t n, size, hsize, crc, sum = 0;
    bool found = false;
    int64_t e;

    if (index == 0 || !blk)
        return ERR_BADARG;
    /* The table is read from the whole device, whatever the view. */
    b->first = 0;
    b->blocks = b->dev_blocks;
    if ((e = blk_read(b, 1, 1, blk)) != 0)
        goto out;
    e = ERR_NOTFOUND;
    hsize = (uint32_t)le(blk + 12, 4);
    if (le(blk, 8) != 0x5452415020494645ULL || hsize < 92 || hsize > bs) {
        e = mbr_range(b, blk, index, first_out, count);     /* not "EFI PART" */
        goto out;
    }
    e = ERR_BADARG;
    crc = (uint32_t)le(blk + 16, 4);
    blk[16] = blk[17] = blk[18] = blk[19] = 0;
    if (crc32(blk, hsize, 0) != crc)
        goto out;
    entries = le(blk + 72, 8);
    n = (uint32_t)le(blk + 80, 4);
    size = (uint32_t)le(blk + 84, 4);
    crc = (uint32_t)le(blk + 88, 4);
    if (size < GPT_ENTRY_MIN || size > bs || (size & (size - 1)) || n == 0 || n > 1024 ||
        entries >= b->dev_blocks)
        goto out;
    tblocks = ((uint64_t)n * size + bs - 1) / bs;
    if (tblocks > b->dev_blocks - entries)
        goto out;
    for (uint64_t k = 0; k < tblocks; k++) {
        uint64_t in = (uint64_t)n * size - k * bs < bs ? (uint64_t)n * size - k * bs : bs;

        if ((e = blk_read(b, entries + k, 1, blk)) != 0)
            goto out;
        sum = crc32(blk, in, sum);
        for (uint64_t at = 0; at < in; at += size) {
            const uint8_t *t = blk + at;

            if ((k * bs + at) / size + 1 != index)
                continue;
            found = !(le(t, 8) == 0 && le(t + 8, 8) == 0);
            first = le(t + 32, 8);
            last = le(t + 40, 8);
        }
    }
    e = ERR_BADARG;
    if (sum != crc)
        goto out;
    e = ERR_NOTFOUND;
    if (!found)
        goto out;                       /* past the table, or an unused entry */
    e = ERR_BADARG;
    if (first > last || last >= b->dev_blocks)
        goto out;
    *first_out = first;
    *count = last - first + 1;
    e = 0;
out:
    b->first = was_first;
    b->blocks = was_blocks;
    return e;
}
