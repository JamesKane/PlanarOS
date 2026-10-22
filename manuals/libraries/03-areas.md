# 3. Exec: memory and Areas

*Checked against V5; pools and `AllocVec`, V6.*

An **Area** is Planar's unit of memory a program can ask for, give to another process, or make executable. Areas are page-rounded and fairly expensive to make, so a program allocates a few large ones and divides them up. A **pool** does the dividing: Exec's V39 pools, and `AllocVec` over a pool of the process's own ([Pools](#pools), below).

## Concepts

- **Allocation.** `AllocArea(size, flags, &address)` rounds `size` up to whole pages, maps the Area read-write into the calling process, and returns its handle, or a negative error. The memory is always zeroed. A size larger than the free memory fails at once with `ERR_NOMEM`, and so can a large one when no single run of free pages is long enough. Each Area's handle counts against the process's 256 (chapter 1, *Limits*).
- **Ownership.** The process that allocated an Area owns it. Only the owner can share it, unshare it, change its rights or free it.
- **Sharing.** `ShareArea(area, task, rights)` maps the Area into the process that `task` belongs to, at the **same address**, with the rights given. A pointer into the Area means the same thing in both processes. The other process must be one the caller has an open, accepted connection with (chapter 4); any other gets `ERR_RIGHTS`.
- **Rights.** Each process that maps an Area has its own rights to it: read, write, execute. Write and execute need read. Execute needs the Area to have been allocated with `AREAF_EXEC`; write *and* execute at once need `AREAF_RWX`.
- **W^X across processes.** Unless the Area is `AREAF_RWX`, it is never writable in one process while executable in another. `ShareArea` or `ProtectArea` that would make it so fails with `ERR_RIGHTS`.
- **Lifetime.** `FreeArea` gives up the owner's handle and its mapping. The memory itself lasts until every process sharing it has dropped it: the owner by `UnshareArea`, the process it was shared with by `ReleaseArea(address)`, or either by ending. A server that was lent an Area by a client lets go of it with `ReleaseArea` when the client is done ([chapter 9](09-queue-leases.md)).
- **Asking.** `QueryArea(address, &info)` says which Area of the caller's an address lies in, and what the caller may do with it.

### Flags

| Flag | Meaning |
|---|---|
| `AREAF_CLEAR` | Zero-filled. Every Area is, so this changes nothing. It's kept for Exec's habit. |
| `AREAF_CONTIG` | Physically contiguous. Every Area is, since its address is its physical address (chapter 1), so this changes nothing. |
| `AREAF_EXEC` | May be made executable with `ProtectArea`, but never while writable. |
| `AREAF_RWX` | May be writable and executable at once. Use only when you have to, such as for a JIT that patches code in place. |
| `AREAF_LARGE` | Aligned to 2 MiB, so the kernel can map it with large pages. |
| `AREAF_DMA32` | Below 4 GiB, for memory a device with 32-bit DMA addresses must reach (V4: SDHCI's ADMA2). |
| `AREAF_NC` | Not cached by the CPU, in every process that maps it; writes to it may be gathered (write-combined). For rings and buffers a device reads without snooping the caches, and framebuffers. Never executable. `ERR_NOSYS` on a CPU that can't (RISC-V without Svpbmt). |

A driver gets device memory differently: a device's registers by `MapLease`, and memory the device reads and writes by `AllocDmaArea`, which takes these flags too ([Devices ch. 1](../devices/01-drivers.md)). DMA Areas are ordinary cacheable memory unless asked for with `AREAF_NC`. On a board whose DMA doesn't snoop the caches (the RV2), the driver cleans and flushes around each transfer, or uses `AREAF_NC` and doesn't. A lease may also limit where its DMA Areas go (`LimitLeaseDma`, set by hwdb).

### Rights

| Right | Meaning |
|---|---|
| `AREAR_READ` | Required in every set of rights |
| `AREAR_WRITE` | |
| `AREAR_EXEC` | Needs `AREAF_EXEC`, or `AREAF_RWX` if `AREAR_WRITE` is also given |

## Using it

### Code written at run time

From `conformance/areas.c`: write code into an Area, then make it executable. W^X holds throughout: the Area is writable or executable, never both.

```c
uint32_t *code;
int64_t a = AllocArea(4096, AREAF_EXEC, (void **)&code);

if (a < 0)
    return a;
emit_code(code);                                    /* writable now */
if (ProtectArea((Handle)a, AREAR_READ | AREAR_EXEC) == 0)   /* executable now; not writable */
    result = ((int (*)(void))(void *)code)();
FreeArea((Handle)a);
```

`ProtectArea` with `AREAR_EXEC` also synchronises the instruction cache over the Area, so no separate cache call is needed.

### Giving a buffer to another process

From `conformance/conn.c`, in outline. The two processes are already connected, and the peer has sent its task's handle (`FindTask(nullptr)`) in a message.

```c
uint8_t *buf;
int64_t a = AllocArea(64 * 1024, 0, (void **)&buf);

fill(buf);
ShareArea((Handle)a, peer_task, AREAR_READ);   /* peer sees it at 'buf', read-only */
/* ... tell the peer the address, in a message ... */
UnshareArea((Handle)a, peer_task);             /* when it's done */
```

The peer receives `buf` as a plain pointer. The usual way to move data between processes is a connection's arena (chapter 4), which both ends already share. Lending an Area for the length of one request (`IOF_LEND`) is **planned**.

## Pools

A **pool** hands out blocks of any size from **puddles**, Areas it allocates as it needs them. A block bigger than the pool's **threshold** is an Area of its own. `DeletePool` frees every Area the pool has, so a set of blocks with one lifetime can go at once, without freeing each.

- **Making one.** `CreatePool(flags, puddleSize, threshSize)` allocates the first puddle, and keeps the pool's header there. The flags are the Areas': `AREAF_DMA32` or `AREAF_NC` for every puddle, `AREAF_CLEAR` to zero every block, and `POOLF_LOCKED` for a pool the process's tasks share. 64 KiB puddles and a 16 KiB threshold suit most programs.
- **In a buffer.** `CreatePoolMem(buf, size)` makes a pool in memory the caller already has, such as part of a connection's heap. It never allocates an Area, and its blocks run out when the buffer does.
- **Blocks.** `AllocPooled(pool, size)` rounds the size up to a class: 16-byte steps to 256 bytes, then four classes to each doubling. It takes a block from that class's free list, or from the current puddle. Blocks are 16-byte aligned and have no header, so `FreePooled(pool, block, size)` needs the size the block was allocated with. A freed block goes back on its class's list. Puddles are kept until `DeletePool`.
- **`AllocVec(size, flags)` and `FreeVec(block)`** are for when the size isn't to hand at the free. Each block has a 16-byte header with its size. They come from a pool the process makes at its first `AllocVec` (64 KiB puddles, 16 KiB threshold, locked), so any task may call them. With `AREAF_DMA32` or `AREAF_NC`, the block is an Area of its own instead.
- **Cost.** A block from a free list or the current puddle takes no kernel call: an `AllocPooled` and `FreePooled` pair measured 95 ns on RV64 and 43 ns on AArch64 under QEMU, and `AllocVec` and `FreeVec`, through the lock, 204 and 78 ns. A new puddle, or a block above the threshold, costs an `AllocArea`, and a handle.

### A pool for one job's blocks

From `conformance/pool.c`, in outline:

```c
struct Pool *p = CreatePool(0, 16 * 1024, 1024);

for (unsigned i = 0; i < BLOCKS; i++)
    block[i] = AllocPooled(p, size_of(i));     /* nullptr if out of memory */
/* ... */
FreePooled(p, block[7], size_of(7));           /* one back early, for reuse */
DeletePool(p);                                 /* and the rest, at once */
```

`conformance/pool.c` checks that the process holds the same handles after `DeletePool` as before `CreatePool`.

## Rules

- **Keep `AllocArea`'s result in an `int64_t`** and test it with `< 0` before using it as a `Handle` (chapter 1).
- **Don't assume where an Area is.** Its address is wherever free memory was, and successive Areas aren't adjacent.
- **A large Area can fail while memory is free.** It needs one run of free pages as long as itself (chapter 1).
- **Sharing is with a process, named by any of its tasks.** Every task in that process can then reach the memory.
- **Share only with a peer.** `ShareArea` needs an open connection to the target. The mapping outlasts the connection, so unshare when the peer is done.
- **A process can map an Area only once.** Sharing it again with the same process fails with `ERR_EXISTS`. To change a sharer's rights, unshare and share again.
- **Freeing doesn't revoke.** Processes the Area was shared with keep it until they drop it, or until it is unshared from them first.
- **Free a pool's block with the size it was allocated with.** The pool has no other record of a small block. Use `AllocVec` where the size isn't to hand.
- **One task at a time in a pool**, unless it was made with `POOLF_LOCKED`. `AllocVec`'s pool is.
- **Don't share a pool's puddles.** A pool's blocks are its own process's. To give memory to another process, allocate an Area for it.

## Differences from Exec

| Exec | Planar |
|---|---|
| `AllocMem(size, MEMF_…)` returns a pointer to any number of bytes | `AllocArea` returns a handle and gives the address separately, in whole pages |
| All memory is visible to every task | An Area is visible only to the processes it's mapped into |
| `MEMF_PUBLIC` for memory other tasks may touch | Share it explicitly with `ShareArea`, and only with a process you're connected to |
| `FreeMem(ptr, size)` | `FreeArea(handle)` |
| `CreatePool(MEMF_…, puddle, threshold)`; `MEMF_SEM_PROTECTED` for a shared pool | `CreatePool(AREAF_…, puddle, threshold)`; `POOLF_LOCKED` |
| `AllocVec(size, MEMF_…)` | `AllocVec(size, AREAF_…)`: `AREAF_CLEAR`, and `AREAF_DMA32` or `AREAF_NC` for an Area of its own |

## Functions used

[AllocArea](../autodocs/exec.md#execallocarea),
[AllocPooled](../autodocs/exec.md#execallocpooled),
[AllocVec](../autodocs/exec.md#execallocvec),
[CreatePool](../autodocs/exec.md#execcreatepool),
[DeletePool](../autodocs/exec.md#execdeletepool),
[FreeArea](../autodocs/exec.md#execfreearea),
[FreePooled](../autodocs/exec.md#execfreepooled),
[FreeVec](../autodocs/exec.md#execfreevec),
[ProtectArea](../autodocs/exec.md#execprotectarea),
[QueryArea](../autodocs/exec.md#execqueryarea),
[ShareArea](../autodocs/exec.md#execsharearea),
[UnshareArea](../autodocs/exec.md#execunsharearea).
