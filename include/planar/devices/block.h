/*
 * planar/devices/block.h -- the block class (docs §11.9, v4-plan step 5)
 *
 * A block driver publishes a port per unit ("nvme.0"). A client connects
 * and sends BlkMsg messages, each a batch of up to BLK_BATCH requests;
 * the driver replies to each message exactly once, when every request
 * in it has finished, each with its own br_Error. Many messages may be
 * outstanding at once: that is the queue depth.
 *
 * The data lies in the connection's heap, which both ends map (VQ3): a
 * request names its buffer by address there, and the driver hands that
 * address to the device as it is, with no copy. The driver checks it
 * lies in the heap. If the driver dies with requests outstanding, the
 * kernel holds the arena until hwdb has stopped the device
 * (planar/lease.h), so the client may close at once.
 *
 * lib/blk is the client library: it reads the GPT, opens partitions,
 * and reconnects and retries reads when the driver restarts.
 */
#ifndef PLANAR_DEVICES_BLOCK_H
#define PLANAR_DEVICES_BLOCK_H

#include <stdint.h>

#include <planar/ipc.h>

constexpr uint32_t BLK_VERSION = 1;
constexpr unsigned BLK_BATCH = 16;

/* BlkMsg.bm_Op */
enum : uint32_t {
    BLK_INFO = 1,           /* bm_Blocks, bm_BlockSize, bm_MaxBlocks out */
    BLK_IO = 2,             /* bm_Count requests in bm_Req */
};

/* BlkReq.br_Op */
enum : uint32_t { BLK_READ = 1, BLK_WRITE = 2, BLK_FLUSH = 3 };

/* br_Error, besides the kernel's ERR_* (ERR_BADARG: a block or buffer out of range) */
constexpr int32_t BLK_EIO = -100;           /* the device failed it */

struct BlkReq {
    uint32_t br_Op;
    int32_t br_Error;               /* out: 0 or why */
    uint64_t br_Block;              /* the first block */
    uint32_t br_Count;              /* blocks, up to bm_MaxBlocks */
    uint32_t br_Pad;
    uint64_t br_Buffer;             /* in the connection's heap, 4-byte aligned */
};

struct BlkMsg {
    struct Message bm_Msg;
    uint32_t bm_Version;            /* BLK_VERSION; mn_Error ERR_BADARG if not */
    uint32_t bm_Op;
    uint32_t bm_Count;
    uint32_t bm_BlockSize;          /* BLK_INFO: bytes */
    uint64_t bm_Blocks;             /* BLK_INFO: how many */
    uint32_t bm_MaxBlocks;          /* BLK_INFO: per request */
    uint32_t bm_ReadOnly;           /* BLK_INFO: writes fail */
    struct BlkReq bm_Req[BLK_BATCH];
};

#endif /* PLANAR_DEVICES_BLOCK_H */
