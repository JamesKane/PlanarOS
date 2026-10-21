/*
 * planar/blk.h -- the block class's client library (lib/blk, docs §11.9)
 *
 * A struct blk is one connection to a block driver's port ("nvme.0"),
 * narrowed to a GPT partition if asked, and the reads and writes on it.
 *
 * Buffers: the device reads and writes the connection's heap directly
 * (planar/devices/block.h). blk_alloc gives heap memory; a transfer to
 * or from it costs no copy. Any other buffer goes through a bounce
 * buffer in the heap, and costs one.
 *
 * When the driver dies the library reconnects, once its port is back,
 * and tries the request again: reads always, writes too, since a block
 * written twice holds the same. A transfer with heap memory can't be
 * retried, since that heap went with the connection: it fails with
 * ERR_PORT_DIED, and the caller makes its heap buffers again (b->gen
 * counts the connections). A bounced one is retried, so a caller that
 * doesn't care for speed never sees a restart.
 */
#ifndef PLANAR_BLK_H
#define PLANAR_BLK_H

#include <stdint.h>

#include <planar/devices/block.h>
#include <planar/msg.h>

constexpr unsigned BLK_MSGS = 32;           /* messages one connection may have out */
constexpr uint64_t BLK_GONE_MS = 3000;      /* how long a restart may take */

struct blk {
    struct MsgPort mp;
    char port[16];
    int64_t sigbit;
    uint64_t heap;                  /* the heap asked for */
    bool open;
    uint32_t gen;                   /* connections made: heap buffers last one */
    uint32_t block_size, max_blocks;
    uint64_t dev_blocks;            /* the device's */
    uint64_t first, blocks;         /* this view's, in the device's blocks */
    bool read_only;
    struct BlkMsg *msg[BLK_MSGS];   /* each connection's, made once */
    uint32_t free;                  /* a bit per msg[] not out */
    uint8_t *bounce;
    uint8_t *scratch;               /* a block, for the partition table */
    uint32_t reconnects;
};

/* Connect to 'port' with 'heap' bytes of heap (at least one bounce buffer
 * and BLK_MSGS messages; planar/ipc.h's CONN_HEAP_MAX at most), and ask
 * what it is. 0, or an error. */
int64_t blk_open(struct blk *b, const char *port, int64_t sigbit, uint64_t heap);
void blk_close(struct blk *b);

/* blk_open, but ERR_NOTFOUND at once if no driver has the port. */
int64_t blk_open_now(struct blk *b, const char *port, int64_t sigbit, uint64_t heap);

/* Narrow b to partition 'index' (from 1) of the GPT on it (UEFI 2.10
 * §5.3), whose header and entries must check. 0, or an error. */
int64_t blk_partition(struct blk *b, unsigned index);

/* Where partition 'index' is, without narrowing b to it: its first block
 * on the device and its length. ERR_NOTFOUND past the table's end, for
 * an unused entry, or with no GPT. */
int64_t blk_partition_range(struct blk *b, unsigned index, uint64_t *first, uint64_t *count);

/* 'size' bytes of this connection's heap, page aligned; nullptr when it's
 * used up, or b reconnected since (make them again). */
void *blk_alloc(struct blk *b, uint64_t size);

/* Synchronous: 'count' blocks from 'block' (in this view), into or from
 * 'buf'. 0, or an error. */
int64_t blk_read(struct blk *b, uint64_t block, uint32_t count, void *buf);
int64_t blk_write(struct blk *b, uint64_t block, uint32_t count, const void *buf);

/* Asynchronous, with heap buffers only: send a read and get its message
 * (nullptr if all BLK_MSGS are out, or on an error), then collect the
 * replies as they come: blk_done gives the next, waiting for it, and
 * blk_release returns it. A death fails what's out with ERR_PORT_DIED. */
struct BlkMsg *blk_submit(struct blk *b, uint64_t block, uint32_t count, void *buf);
struct BlkMsg *blk_done(struct blk *b);
void blk_release(struct blk *b, struct BlkMsg *m);
unsigned blk_out(const struct blk *b);

#endif /* PLANAR_BLK_H */
