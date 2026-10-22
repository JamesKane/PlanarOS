# Autodocs

*Planar Reference Manuals, volume 3.* One entry per call, in the RKM's format. The format is in [STYLE.md](../STYLE.md#autodocs).

| File | Contents |
|---|---|
| [exec.md](exec.md) | The kernel calls (`planar/exec.h`, `planar/ipc.h`) |
| [msg.md](msg.md) | Exec's messages over connections (`planar/msg.h`, `lib/exec`) |
| [lease.md](lease.md) | Device leases: registers, interrupt lines, MSIs, DMA Areas, the firmware's tables and the console's input (`planar/lease.h`) |
| [drv.md](drv.md) | drv.library, a driver's main loop (`planar/drv.h`, `lib/drv`) |
| [blk.md](blk.md) | The block class's client library (`planar/blk.h`, `lib/blk`) |
| [dos.md](dos.md) | dos.library: files, the loader, the console (`planar/dos.h`, `lib/dos`) |

## All calls

| Call | Where | |
|---|---|---|
| AcceptConn | [exec](exec.md#execacceptconn) | kernel |
| AcceptPort | [msg](msg.md#execacceptport) | library |
| AckWaitIrq | [lease](lease.md#leaseackwaitirq) | kernel |
| AllocArea | [exec](exec.md#execallocarea) | kernel |
| AllocDmaArea | [lease](lease.md#leaseallocdmaarea) | kernel |
| AllocMsg | [msg](msg.md#execallocmsg) | library |
| AllocMsi | [lease](lease.md#leaseallocmsi) | kernel |
| AllocPooled | [exec](exec.md#execallocpooled) | library |
| AllocSignal | [exec](exec.md#execallocsignal) | kernel |
| AllocVec | [exec](exec.md#execallocvec) | library |
| ArmPort | [msg](msg.md#execarmport) | library |
| ArmTimer | [exec](exec.md#execarmtimer) | kernel |
| BindIrq | [lease](lease.md#leasebindirq) | kernel |
| blk_alloc | [blk](blk.md#blkblk_alloc) | library |
| blk_close | [blk](blk.md#blkblk_close) | library |
| blk_done | [blk](blk.md#blkblk_done) | library |
| blk_open, blk_open_now | [blk](blk.md#blkblk_open) | library |
| blk_out | [blk](blk.md#blkblk_out) | library |
| blk_partition, blk_partition_range | [blk](blk.md#blkblk_partition) | library |
| blk_read, blk_write | [blk](blk.md#blkblk_read) | library |
| blk_release | [blk](blk.md#blkblk_release) | library |
| blk_submit | [blk](blk.md#blkblk_submit) | library |
| CloseConn | [exec](exec.md#execcloseconn) | kernel |
| CloseHandle | [exec](exec.md#execclosehandle) | kernel |
| ClosePort | [msg](msg.md#execcloseport) | library |
| con_open, con_write, con_puts, con_dec, con_close | [dos](dos.md#doscon_open) | library |
| ConnectPort | [msg](msg.md#execconnectport) | library |
| CreateLease | [lease](lease.md#leasecreatelease) | kernel |
| CopyMem | [exec](exec.md#execcopymem) | library |
| CopyMemQuick | [exec](exec.md#execcopymemquick) | library |
| CreatePool, CreatePoolMem | [exec](exec.md#execcreatepool) | library |
| CreatePort | [exec](exec.md#execcreateport) | kernel |
| CreateProcess | [exec](exec.md#execcreateprocess) | kernel |
| CreateProcessImage | [exec](exec.md#execcreateprocessimage) | kernel |
| CreateTask | [exec](exec.md#execcreatetask) | library |
| CurrentTime | [exec](exec.md#execcurrenttime) | kernel |
| DeletePool | [exec](exec.md#execdeletepool) | library |
| DeletePort | [exec](exec.md#execdeleteport) | kernel |
| DoMsg | [msg](msg.md#execdomsg) | library |
| dos_args, dos_path | [dos](dos.md#dosdos_args) | library |
| dos_file, dos_close_file | [dos](dos.md#dosdos_file) | library |
| dos_mkdir, dos_remove | [dos](dos.md#dosdos_mkdir) | library |
| dos_next | [dos](dos.md#dosdos_next) | library |
| dos_open, dos_close | [dos](dos.md#dosdos_open) | library |
| dos_read, dos_write | [dos](dos.md#dosdos_read) | library |
| dos_run | [dos](dos.md#dosdos_run) | library |
| dos_stat | [dos](dos.md#dosdos_stat) | library |
| dos_volume | [dos](dos.md#dosdos_volume) | library |
| drv_dma_clean, drv_dma_flush | [drv](drv.md#drvdrv_dma_clean) | library |
| drv_hwdb_msg, drv_hwdb | [drv](drv.md#drvdrv_hwdb) | library |
| drv_in_heap | [drv](drv.md#drvdrv_in_heap) | library |
| drv_lease, drv_wait_irq | [drv](drv.md#drvdrv_lease) | library |
| drv_log | [drv](drv.md#drvdrv_log) | library |
| drv_main | [drv](drv.md#drvdrv_main) | library |
| drv_publish | [drv](drv.md#drvdrv_publish) | library |
| drv_reply | [drv](drv.md#drvdrv_reply) | library |
| ExitProcess | [exec](exec.md#execexitprocess) | kernel |
| ExitTask | [exec](exec.md#execexittask) | kernel |
| FindTask | [exec](exec.md#execfindtask) | kernel |
| FlushPort | [msg](msg.md#execflushport) | library |
| FreeArea | [exec](exec.md#execfreearea) | kernel |
| FreePooled | [exec](exec.md#execfreepooled) | library |
| FreeSignal | [exec](exec.md#execfreesignal) | kernel |
| FreeVec | [exec](exec.md#execfreevec) | library |
| GetMsg | [msg](msg.md#execgetmsg) | library |
| GetMsgSized | [msg](msg.md#execgetmsgsized) | library |
| GrantConsole | [lease](lease.md#leasegrantconsole) | kernel |
| GrantFirmware | [lease](lease.md#leasegrantfirmware) | kernel |
| GrantLease | [lease](lease.md#leasegrantlease) | kernel |
| KillProcess | [exec](exec.md#execkillprocess) | kernel |
| KLog | [exec](exec.md#execklog) | kernel |
| MapFirmware | [lease](lease.md#leasemapfirmware) | kernel |
| MapLease | [lease](lease.md#leasemaplease) | kernel |
| OpenPort | [exec](exec.md#execopenport) | kernel |
| PortGone | [msg](msg.md#execportgone) | library |
| ProtectArea | [exec](exec.md#execprotectarea) | kernel |
| PutMsg | [msg](msg.md#execputmsg) | library |
| QueryArea | [exec](exec.md#execqueryarea) | kernel |
| QueryLease | [lease](lease.md#leasequerylease) | kernel |
| QueryProcess | [exec](exec.md#execqueryprocess) | kernel |
| QuerySystem | [exec](exec.md#execquerysystem) | kernel |
| RawDoFmt | [exec](exec.md#execrawdofmt) | library |
| ReadConsole | [lease](lease.md#leasereadconsole) | kernel |
| ReplyMsg | [msg](msg.md#execreplymsg) | library |
| ReplyMsgLater | [msg](msg.md#execreplymsglater) | library |
| RevokeLease | [lease](lease.md#leaserevokelease) | kernel |
| SetSignal | [exec](exec.md#execsetsignal) | kernel |
| SetMem | [exec](exec.md#execsetmem) | library |
| ShareArea | [exec](exec.md#execsharearea) | kernel |
| Signal | [exec](exec.md#execsignal) | kernel |
| SignalConn | [exec](exec.md#execsignalconn) | kernel |
| SignalWaitConn | [exec](exec.md#execsignalwaitconn) | kernel |
| StartProcess | [exec](exec.md#execstartprocess) | kernel |
| UnshareArea | [exec](exec.md#execunsharearea) | kernel |
| Wait | [exec](exec.md#execwait) | kernel |
| WaitPort | [msg](msg.md#execwaitport) | library |
| WaitPorts | [msg](msg.md#execwaitports) | library |

## Known problems found while writing these

Each is under BUGS in its entry.
- `FindTask` still returns `Handle`, so its error comes back as a large unsigned value. (`AllocArea` and `CreateTask` now return `int64_t`.)
- `ExitTask`'s code goes nowhere.
- `SignalWaitConn` returns 0 at once for a bad handle, the same as for a closed peer.
- An over-long port or program name (`CreatePort`, `CreateProcess`) gives `ERR_BADADDR` rather than `ERR_BADARG`.
- If `OpenPort`'s or `AcceptConn`'s `arena` pointer stops being writable mid-call, the connection is made but its handle is lost.
- `KLog` drops lines over its rate limit and still returns 0, and the console is still polled.
- `AckWaitIrq` reports errors only when its mask is 0. With a nonzero mask a refused call returns 0 at once.
- `MapFirmware` leaves what it mapped in place when it fails part way.
- `AllocMsi` makes the sources, and counts the device as given, before it writes `info`; an unwritable `info` loses them.
- `CreateProcessImage` takes the Area before it makes the process, so an `ERR_NOMEM` loses the Area.
- `ReadConsole` polls the UART: there's no receive interrupt, and what's typed while nobody reads can be lost.
