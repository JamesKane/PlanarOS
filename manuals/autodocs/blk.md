# blk: the block class's client library

*Checked against V5.*

The calls a program uses to read and write a block device, in alphabetical order. The header is `planar/blk.h`; the code is `lib/blk/blk.c`, linked into the programs that need it (hwdb, `dos`, `blk*` conformance programs: `mk/user.mk`). None is a kernel call, but each one that talks to the driver sends messages, which trap when the driver is asleep. The protocol is in [Devices ch. 2](../devices/02-block.md).

A `struct blk` is one connection to a driver's port ("nvme.0", "sd.0"), and a **view** of the device: the whole of it, or a partition (GPT, or MBR on a disk without one). Block numbers given to the calls are the view's.

Buffers:
- **Heap buffers**, from `blk_alloc`, are in the connection's heap, which the driver hands the device as they are. A transfer with one costs no copy. It can't be retried across a driver restart, since that heap goes with the connection.
- **Any other buffer** goes through a bounce buffer in the heap, a piece at a time, and costs a copy. It's retried across a restart, for up to `BLK_GONE_MS` (3 s).

`b->gen` counts the connections made; heap buffers belong to one. `b->reconnects` counts the restarts seen.

---

## blk/blk_alloc

**NAME**
blk_alloc -- heap memory for zero-copy transfers

**SYNOPSIS**
```c
#include <planar/blk.h>
void *blk_alloc(struct blk *b, uint64_t size);
```

**FUNCTION**
Gives `size` bytes of the connection's heap, page aligned. There's no freeing: allocate a connection's buffers once.

**RESULT**
The memory, or `nullptr` when the heap is used up, `size` is 0, or `b` isn't open.

**NOTES**
After a reconnect (`b->gen` changed) earlier pointers are dead: allocate again.

**BUGS**
None known.

**SEE ALSO**
[blk_open](#blkblk_open), [blk_submit](#blkblk_submit)

---

## blk/blk_close

**NAME**
blk_close -- close the connection

**SYNOPSIS**
```c
#include <planar/blk.h>
void blk_close(struct blk *b);
```

**FUNCTION**
Closes the connection. Messages still out are abandoned; the driver answers them to nobody.

**BUGS**
None known.

**SEE ALSO**
[blk_open](#blkblk_open)

---

## blk/blk_done

**NAME**
blk_done -- the next asynchronous reply

**SYNOPSIS**
```c
#include <planar/blk.h>
struct BlkMsg *blk_done(struct blk *b);
```

**FUNCTION**
Waits for the next reply to a message `blk_submit` sent, and returns it. Its result is in `bm_Msg.mn_Error` and `bm_Req[0].br_Error`. Give it back with `blk_release`.

**RESULT**
The message, or `nullptr` once the driver has gone: everything out has failed. Release them, and reopen.

**BUGS**
None known.

**SEE ALSO**
[blk_submit](#blkblk_submit), [blk_release](#blkblk_release)

---

## blk/blk_open

**NAME**
blk_open, blk_open_now -- connect to a block driver

**SYNOPSIS**
```c
#include <planar/blk.h>
int64_t blk_open(struct blk *b, const char *port, int64_t sigbit, uint64_t heap);
int64_t blk_open_now(struct blk *b, const char *port, int64_t sigbit, uint64_t heap);
```

**FUNCTION**
Connects to `port`, with `heap` bytes of connection heap and signal bit `sigbit`, and asks the driver what the device is (`BLK_INFO`). The view is the whole device.

`blk_open` waits up to `BLK_GONE_MS` (3 s) for the port, which a restarting driver republishes, and through a death while it asks. `blk_open_now` gives `ERR_NOTFOUND` at once if no driver has the port (`dos` uses it to find what's there).

**INPUTS**
- `port` -- the class port, "nvme.0".
- `sigbit` -- the connection's signal bit.
- `heap` -- enough for 32 messages, a bounce buffer (up to 128 KiB) and a block, plus what `blk_alloc` will give; at most 1 MiB (`CONN_HEAP_MAX`).

**RESULT**
0, or:
- `ERR_NOTFOUND` -- no such port (after the wait, for `blk_open`).
- `ERR_PORT_DIED` -- the driver kept dying for 3 s.
- `ERR_NOMEM` -- the heap is too small.
- `ERR_BADARG` -- the driver's answer made no sense: a block size not a power of two from 512 to 65536, no blocks, or no transfer size. On a reconnect, also a device that isn't the one it was.
- Any error `ConnectPort` gives.

**BUGS**
None known.

**SEE ALSO**
[blk_partition](#blkblk_partition), [blk_close](#blkblk_close)

---

## blk/blk_out

**NAME**
blk_out -- messages out

**SYNOPSIS**
```c
#include <planar/blk.h>
unsigned blk_out(const struct blk *b);
```

**FUNCTION**
How many of the connection's 32 messages (`BLK_MSGS`) are taken: sent and not yet released.

**BUGS**
None known.

---

## blk/blk_partition

**NAME**
blk_partition, blk_partition_range -- a partition

**SYNOPSIS**
```c
#include <planar/blk.h>
int64_t blk_partition(struct blk *b, unsigned index);
int64_t blk_partition_range(struct blk *b, unsigned index, uint64_t *first, uint64_t *count);
```

**FUNCTION**
Reads the GPT (UEFI 2.10 §5.3) from the whole device, checks its header's and entries' CRCs, and finds partition `index` (from 1). A device with no GPT is read as an MBR instead, as SD cards often are: its primary partitions are 1 to 4. A protective MBR (type `EE`) without its GPT is no table. `blk_partition` narrows `b`'s view to it. `blk_partition_range` gives where it is, its first block and length, and leaves the view as it was.

**RESULT**
0, or:
- `ERR_NOTFOUND` -- no GPT and no MBR, `index` past the table (past 4 for an MBR), or an unused entry.
- `ERR_BADARG` -- `index` is 0, a CRC doesn't check, or the table or the partition doesn't fit the device.
- Any error a read gives.

**NOTES**
- Partitions are this library's, not a server's: every client of a disk can reach all of it. [V4 step 5](../../docs/v4-plan.md) says why.
- The table is read a block at a time through the connection's own scratch block, so it uses no more heap.

**BUGS**
None known.

**SEE ALSO**
[blk_open](#blkblk_open)

---

## blk/blk_read

**NAME**
blk_read, blk_write -- synchronous transfers

**SYNOPSIS**
```c
#include <planar/blk.h>
int64_t blk_read(struct blk *b, uint64_t block, uint32_t count, void *buf);
int64_t blk_write(struct blk *b, uint64_t block, uint32_t count, const void *buf);
```

**FUNCTION**
Reads or writes `count` blocks from `block` of the view, waiting for the result. A heap buffer goes to the device as it is, in one request of at most `b->max_blocks`. Any other buffer goes through the bounce buffer, as many requests as it takes, each retried across a driver restart.

**RESULT**
0, or:
- `ERR_BADARG` -- `count` is 0, the range leaves the view, or a heap transfer is over `max_blocks`; or the driver refused the request.
- `ERR_PORT_DIED` -- with a heap buffer, the driver died: the library has reconnected, the buffer is dead, and the caller allocates and tries again. With a bounced one, the driver stayed dead for 3 s.
- `ERR_RIGHTS` -- `blk_write` to a read-only device.
- `BLK_EIO` -- the device failed it.
- `ERR_FULL` -- all 32 messages are out (asynchronous ones).

**EXAMPLE**
The kill test's client, from `servers/hwdb/killtest.c`, reads through deaths with a bounced buffer:
```c
int64_t e = blk_read(&b, at, n, buf);

if (e != 0) {
    c->bad++;
    ...
}
if (b.reconnects != seen)       /* the driver died and came back meanwhile */
    c->deaths += b.reconnects - seen;
```

**NOTES**
A write retried after a restart writes the same blocks again, which leaves the same data.

**BUGS**
None known.

**SEE ALSO**
[blk_alloc](#blkblk_alloc), [blk_submit](#blkblk_submit)

---

## blk/blk_release

**NAME**
blk_release -- give back an asynchronous message

**SYNOPSIS**
```c
#include <planar/blk.h>
void blk_release(struct blk *b, struct BlkMsg *m);
```

**FUNCTION**
Returns `m`, from `blk_submit`, to the connection's free messages, once its reply has come (or the driver has gone).

**BUGS**
None known.

---

## blk/blk_submit

**NAME**
blk_submit -- send a read and don't wait

**SYNOPSIS**
```c
#include <planar/blk.h>
struct BlkMsg *blk_submit(struct blk *b, uint64_t block, uint32_t count, void *buf);
```

**FUNCTION**
Sends a read of `count` blocks from `block` into `buf`, a heap buffer, and returns its message at once. Up to 32 can be out: that's the queue depth. `blk_done` collects the replies.

**RESULT**
The message, or `nullptr`: all 32 are out, the range leaves the view, `count` is over `max_blocks`, `buf` isn't in the heap, or the send failed.

**EXAMPLE**
From `conformance/blkbench.c`, keeping the queue at its depth:
```c
while (out < depth) {
    io[i].m = blk_submit(&b, io[i].block, per, io[i].buf);
    out++;
}
struct BlkMsg *m = blk_done(&b);
...
blk_release(&b, m);
out--;
```

**BUGS**
Reads only; there's no asynchronous write yet.

**SEE ALSO**
[blk_done](#blkblk_done), [blk_release](#blkblk_release), [blk_out](#blkblk_out)
