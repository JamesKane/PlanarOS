# The block class: NVMe, SD and lib/blk

*Checked against V4.*

## What it's for

Reading and writing disks by block. The NVMe driver (`drv/nvme`) and the SD driver (`drv/sdhci`) both serve it, so a program doesn't care which it has. Most programs use files instead ([DOS](../dos/README.md)); `dos` itself is a block client.

## Concepts

A block driver publishes a port per device: "nvme.0", "sd.0". The protocol is in `planar/devices/block.h`:

- A client sends **`struct BlkMsg`** messages. `bm_Op` is `BLK_INFO` (the driver fills in `bm_Blocks`, `bm_BlockSize`, `bm_MaxBlocks`, `bm_ReadOnly`) or `BLK_IO`, a batch of up to 16 requests (`BLK_BATCH`) in `bm_Req`.
- Each **`struct BlkReq`** is `BLK_READ`, `BLK_WRITE` or `BLK_FLUSH`, a first block, a count (up to `bm_MaxBlocks`), and a buffer. Its result comes back in `br_Error`.
- The driver answers a message exactly once, when every request in it has finished. Many messages may be out at once: that's the queue depth. `bm_Version` must be `BLK_VERSION`.
- **The data is in the connection's heap**, which both ends map. A request names its buffer by address; the driver checks it lies in the heap and hands it to the device as it is. No copy (VQ3: an Area's address is its physical address).

Errors in `br_Error` or `mn_Error`: 0; `ERR_BADARG` (a block or buffer out of range, a bad op); `ERR_FULL` (more messages out than the driver will hold, 64); `BLK_EIO` (the device failed it).

**If the driver dies with requests out**, the kernel holds the client's arena until hwdb has stopped the device ([chapter 1](01-drivers.md#when-a-driver-dies)). The client can close its connection at once; the memory isn't reused while the device might write it.

## Using it

`lib/blk` (`planar/blk.h`) is the client library. Open a port, choose a partition, read:

```c
struct blk b;

if (blk_open(&b, "nvme.0", SIG_BLK, 1 << 20) != 0)
    return 1;
blk_partition(&b, 2);           /* the second partition; block 0 is its first */
blk_read(&b, 0, 8, buf);
```

Two kinds of buffer:
- **Heap buffers** from `blk_alloc` cost no copy. `conformance/blkbench.c` keeps 32 reads out with `blk_submit` and `blk_done`, each into its own heap buffer.
- **Any other buffer** is copied through a bounce buffer in the heap. Slower, but retried across a restart.

### Restarts

When the driver dies, the library reconnects once its port is back, waiting up to 3 s (`BLK_GONE_MS`):
- A bounced `blk_read` or `blk_write` is tried again. A caller that doesn't care for speed never sees a restart.
- A transfer with a heap buffer can't be: that heap went with the connection. It fails with `ERR_PORT_DIED`; the caller allocates its buffers again (`b->gen` has changed) and retries.
- Asynchronous reads out at the death fail: `blk_done` gives `nullptr`.

## Measuring it

hwdb's test mode runs `blkbench` on each block port: 4 KiB random reads at depths 1, 2, 8 and 32, with latency and, from `QuerySystem`, the kernel calls, interrupts and crossings per I/O. On the test disk (`tools/mkdisk.py`) every block holds its own number, and each read is checked. The CIX's numbers are in the [V4 plan](../../docs/v4-plan.md).

## Rules

- Keep buffers in the heap for speed, or anywhere for simplicity; never pass an address outside the heap in a message yourself: the driver refuses it.
- After `ERR_PORT_DIED` with a heap buffer, allocate again before reusing it.
- Writes retried after a restart write the same blocks again. Order across a restart isn't promised.
- Partitions are `lib/blk`'s, not a server's: every client of a disk can reach all of it.
- A disk's partitions come from its GPT, or from its MBR when it has none (the RV2's SD card: primary partitions 1 to 4).

## Differences from Exec

- No `CMD_READ` on an `IOStdReq`: a `BlkMsg` carries up to 16 requests.
- The buffer isn't the caller's own memory but the connection's heap, or a copy goes through it.

## Functions used

[blk_open](../autodocs/blk.md#blkblk_open), [blk_partition](../autodocs/blk.md#blkblk_partition), [blk_alloc](../autodocs/blk.md#blkblk_alloc), [blk_read](../autodocs/blk.md#blkblk_read), [blk_submit](../autodocs/blk.md#blkblk_submit), [blk_done](../autodocs/blk.md#blkblk_done), [blk_release](../autodocs/blk.md#blkblk_release), [QuerySystem](../autodocs/exec.md#execquerysystem)
