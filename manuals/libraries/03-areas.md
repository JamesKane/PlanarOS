# 3. Exec: memory and Areas

*Checked against V3.*

An **Area** is Planar's unit of memory a program can ask for, give to another process, or make executable. Areas are page-rounded and fairly expensive to make, so a program allocates a few large ones and divides them up itself. (A `malloc`-style allocator over Areas is **planned** as part of the C runtime.)

## Concepts

- **Allocation.** `AllocArea(size, flags, &address)` rounds `size` up to whole pages, maps the Area read-write into the calling process, and returns its handle, or a negative error. The memory is always zeroed. A size larger than the free memory fails at once with `ERR_NOMEM`, and so can a large one when no single run of free pages is long enough. Each Area's handle counts against the process's 256 (chapter 1, *Limits*).
- **Ownership.** The process that allocated an Area owns it. Only the owner can share it, unshare it, change its rights or free it.
- **Sharing.** `ShareArea(area, task, rights)` maps the Area into the process that `task` belongs to, at the **same address**, with the rights given. A pointer into the Area means the same thing in both processes. The other process must be one the caller has an open, accepted connection with (chapter 4); any other gets `ERR_RIGHTS`.
- **Rights.** Each process that maps an Area has its own rights to it: read, write, execute. Write and execute need read. Execute needs the Area to have been allocated with `AREAF_EXEC`; write *and* execute at once need `AREAF_RWX`.
- **W^X across processes.** Unless the Area is `AREAF_RWX`, it is never writable in one process while executable in another. `ShareArea` or `ProtectArea` that would make it so fails with `ERR_RIGHTS`.
- **Lifetime.** `FreeArea` gives up the owner's handle and its mapping. The memory itself lasts until every process sharing it has dropped it (by `UnshareArea`, or by ending).
- **Asking.** `QueryArea(address, &info)` says which Area of the caller's an address lies in, and what the caller may do with it.

### Flags

| Flag | Meaning |
|---|---|
| `AREAF_CLEAR` | Zero-filled. Every Area is, so this changes nothing. It's kept for Exec's habit. |
| `AREAF_CONTIG` | Physically contiguous. Every Area is, since its address is its physical address (chapter 1), so this changes nothing. |
| `AREAF_EXEC` | May be made executable with `ProtectArea`, but never while writable. |
| `AREAF_RWX` | May be writable and executable at once. Use only when you have to, such as for a JIT that patches code in place. |
| `AREAF_LARGE` | Aligned to 2 MiB, so the kernel can map it with large pages. |

The flags for device memory, non-cacheable and write-combining memory, and DMA below 4 GiB are **planned** with drivers ([docs §3.3](../../docs/03-memory.md)).

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

The peer receives `buf` as a plain pointer. In V2 the usual way to move data between processes is a connection's arena (chapter 4), which both ends already share. Lending an Area for the length of one request (`IOF_LEND`) is **planned**.

## Rules

- **Keep `AllocArea`'s result in an `int64_t`** and test it with `< 0` before using it as a `Handle` (chapter 1).
- **Don't assume where an Area is.** Its address is wherever free memory was, and successive Areas aren't adjacent.
- **A large Area can fail while memory is free.** It needs one run of free pages as long as itself (chapter 1).
- **Sharing is with a process, named by any of its tasks.** Every task in that process can then reach the memory.
- **Share only with a peer.** `ShareArea` needs an open connection to the target. The mapping outlasts the connection, so unshare when the peer is done.
- **A process can map an Area only once.** Sharing it again with the same process fails with `ERR_EXISTS`. To change a sharer's rights, unshare and share again.
- **Freeing doesn't revoke.** Processes the Area was shared with keep it until they drop it, or until it is unshared from them first.

## Differences from Exec

| Exec | Planar |
|---|---|
| `AllocMem(size, MEMF_…)` returns a pointer to any number of bytes | `AllocArea` returns a handle and gives the address separately, in whole pages |
| All memory is visible to every task | An Area is visible only to the processes it's mapped into |
| `MEMF_PUBLIC` for memory other tasks may touch | Share it explicitly with `ShareArea`, and only with a process you're connected to |
| `FreeMem(ptr, size)` | `FreeArea(handle)` |

## Functions used

[AllocArea](../autodocs/exec.md#execallocarea),
[FreeArea](../autodocs/exec.md#execfreearea),
[ProtectArea](../autodocs/exec.md#execprotectarea),
[QueryArea](../autodocs/exec.md#execqueryarea),
[ShareArea](../autodocs/exec.md#execsharearea),
[UnshareArea](../autodocs/exec.md#execunsharearea).
