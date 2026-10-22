# Autodocs

*Planar Reference Manuals, volume 3.* One entry per call, in the RKM's format. The format is in [STYLE.md](../STYLE.md#autodocs).

| File | Contents |
|---|---|
| [exec.md](exec.md) | The kernel calls (`planar/exec.h`, `planar/ipc.h`) |
| [msg.md](msg.md) | Exec's messages over connections (`planar/msg.h`, `lib/exec`) |
| [lease.md](lease.md) | Device leases: registers, interrupt lines, MSIs, DMA Areas, the firmware's tables, the console's input and the kernel log (`planar/lease.h`) |
| [drv.md](drv.md) | drv.library, a driver's main loop (`planar/drv.h`, `lib/drv`) |
| [blk.md](blk.md) | The block class's client library (`planar/blk.h`, `lib/blk`) |
| [dos.md](dos.md) | dos.library: files, the loader, `ReadArgs` and paths, the console and `Printf` (`planar/dos.h`, `lib/dos`) |
| [utility.md](utility.md) | utility.library: tag lists, hooks, strings, `Span` and `Str` (`planar/utility.h`, `lib/utility`) |
| [gfx.md](gfx.md) | gfx.library: bitmaps, rastports and drawing (`planar/gfx.h`, `lib/gfx`) |

## All calls

| Call | Where | |
|---|---|---|
| AcceptConn | [exec](exec.md#execacceptconn) | kernel |
| AcceptPort | [msg](msg.md#execacceptport) | library |
| AckWaitIrq | [lease](lease.md#leaseackwaitirq) | kernel |
| AddPart, FilePart, PathPart | [dos](dos.md#dosaddpart) | library |
| AllocArea | [exec](exec.md#execallocarea) | kernel |
| AllocBitMap | [gfx](gfx.md#gfxallocbitmap) | library, through `IGfx` |
| AllocDmaArea | [lease](lease.md#leaseallocdmaarea) | kernel |
| AllocMsg | [msg](msg.md#execallocmsg) | library |
| AllocMsi | [lease](lease.md#leaseallocmsi) | kernel |
| AllocPooled | [exec](exec.md#execallocpooled) | library |
| AllocSignal | [exec](exec.md#execallocsignal) | kernel |
| AllocVec | [exec](exec.md#execallocvec) | library |
| ArmPort | [msg](msg.md#execarmport) | library |
| ArmTimer | [exec](exec.md#execarmtimer) | kernel |
| BltBitMap | [gfx](gfx.md#gfxbltbitmap) | library, through `IGfx` |
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
| CallHookPkt | [utility](utility.md#utilitycallhookpkt) | library |
| CloseConn | [exec](exec.md#execcloseconn) | kernel |
| CloseHandle | [exec](exec.md#execclosehandle) | kernel |
| CloseLibrary | [exec](exec.md#execcloselibrary) | library, through `IExec` |
| ClosePort | [msg](msg.md#execcloseport) | library |
| ColdReboot | [lease](lease.md#leasecoldreboot) | kernel |
| ConnectPort, ConnectPortUntil | [msg](msg.md#execconnectport) | library |
| CreateDir, DeletePath | [dos](dos.md#doscreatedir) | library |
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
| DoMsgUntil | [msg](msg.md#execdomsguntil) | library |
| DropInterface | [exec](exec.md#execdropinterface) | library, through `IExec` |
| DrawBevel | [gfx](gfx.md#gfxdrawbevel) | library, through `IGfx` |
| drv_dma_clean, drv_dma_flush | [drv](drv.md#drvdrv_dma_clean) | library |
| drv_hwdb_msg, drv_hwdb | [drv](drv.md#drvdrv_hwdb) | library |
| drv_in_heap | [drv](drv.md#drvdrv_in_heap) | library |
| drv_lease, drv_wait_irq | [drv](drv.md#drvdrv_lease) | library |
| drv_log | [drv](drv.md#drvdrv_log) | library |
| drv_main | [drv](drv.md#drvdrv_main) | library |
| drv_publish | [drv](drv.md#drvdrv_publish) | library |
| drv_reply | [drv](drv.md#drvdrv_reply) | library |
| ExamineNext | [dos](dos.md#dosexaminenext) | library |
| ExaminePath | [dos](dos.md#dosexaminepath) | library |
| ExitProcess | [exec](exec.md#execexitprocess) | kernel |
| ExitTask | [exec](exec.md#execexittask) | kernel |
| FindTagItem | [utility](utility.md#utilityfindtagitem) | library |
| FindTask | [exec](exec.md#execfindtask) | kernel |
| FlushPort | [msg](msg.md#execflushport) | library |
| FreeArea | [exec](exec.md#execfreearea) | kernel |
| FreeBitMap | [gfx](gfx.md#gfxfreebitmap) | library, through `IGfx` |
| FreePooled | [exec](exec.md#execfreepooled) | library |
| FreeSignal | [exec](exec.md#execfreesignal) | kernel |
| FreeVec | [exec](exec.md#execfreevec) | library |
| GetBitMapAttr | [gfx](gfx.md#gfxgetbitmapattr) | library, through `IGfx` |
| GetInterface | [exec](exec.md#execgetinterface) | library, through `IExec` |
| GetMsg | [msg](msg.md#execgetmsg) | library |
| GetMsgSized | [msg](msg.md#execgetmsgsized) | library |
| GetTagData | [utility](utility.md#utilitygettagdata) | library |
| GrantConsole | [lease](lease.md#leasegrantconsole) | kernel |
| GrantFirmware | [lease](lease.md#leasegrantfirmware) | kernel |
| GrantLease | [lease](lease.md#leasegrantlease) | kernel |
| GrantRoot | [lease](lease.md#leasegrantroot) | kernel |
| InitBitMap | [gfx](gfx.md#gfxinitbitmap) | library, through `IGfx` |
| InitRastPort | [gfx](gfx.md#gfxinitrastport) | library, through `IGfx` |
| KillProcess | [exec](exec.md#execkillprocess) | kernel |
| KLog | [exec](exec.md#execklog) | kernel |
| MapFirmware | [lease](lease.md#leasemapfirmware) | kernel |
| MapLease | [lease](lease.md#leasemaplease) | kernel |
| NextTagItem | [utility](utility.md#utilitynexttagitem) | library |
| NextVolume | [dos](dos.md#dosnextvolume) | library |
| OpenCon, WriteCon, PutStr, CloseCon | [dos](dos.md#dosopencon) | library |
| OpenDos, OpenDosUntil, CloseDos | [dos](dos.md#dosopendos) | library |
| OpenFile, CloseFile | [dos](dos.md#dosopenfile) | library |
| OpenLibrary | [exec](exec.md#execopenlibrary) | library, through `IExec` |
| OpenPort | [exec](exec.md#execopenport) | kernel |
| PortGone | [msg](msg.md#execportgone) | library |
| Printf, VPrintf | [dos](dos.md#dosprintf) | library |
| ProtectArea | [exec](exec.md#execprotectarea) | kernel |
| PutMsg | [msg](msg.md#execputmsg) | library |
| QueryArea | [exec](exec.md#execqueryarea) | kernel |
| QueryLease | [lease](lease.md#leasequerylease) | kernel |
| QueryProcess | [exec](exec.md#execqueryprocess) | kernel |
| QuerySystem | [exec](exec.md#execquerysystem) | kernel |
| QueryTasks | [exec](exec.md#execquerytasks) | kernel |
| RawDoFmt | [exec](exec.md#execrawdofmt) | library |
| RectFill | [gfx](gfx.md#gfxrectfill) | library, through `IGfx` |
| ReadArgs, FreeArgs | [dos](dos.md#dosreadargs) | library |
| ReadConsole | [lease](lease.md#leasereadconsole) | kernel |
| ReadFile, WriteFile | [dos](dos.md#dosreadfile) | library |
| ReadLog | [lease](lease.md#leasereadlog) | kernel |
| ReplyMsg | [msg](msg.md#execreplymsg) | library |
| ReplyMsgLater | [msg](msg.md#execreplymsglater) | library |
| ReportUnresponsive | [msg](msg.md#execreportunresponsive) | library, in `libexec.a` |
| RevokeLease | [lease](lease.md#leaserevokelease) | kernel |
| RunCommand | [dos](dos.md#dosruncommand) | library |
| SetSignal | [exec](exec.md#execsetsignal) | kernel |
| SetMem | [exec](exec.md#execsetmem) | library |
| ShareArea | [exec](exec.md#execsharearea) | kernel |
| Signal | [exec](exec.md#execsignal) | kernel |
| SignalConn | [exec](exec.md#execsignalconn) | kernel |
| SignalWaitConn | [exec](exec.md#execsignalwaitconn) | kernel |
| SNPrintf, VSNPrintf | [utility](utility.md#utilitysnprintf) | library |
| SpanReadU8, SpanReadU16LE … SpanReadU64BE | [utility](utility.md#utilityspanread) | library |
| SpanSlice | [utility](utility.md#utilityspanslice) | library |
| SplitArgs | [dos](dos.md#dossplitargs) | library |
| StartProcess | [exec](exec.md#execstartprocess) | kernel |
| StrFromC | [utility](utility.md#utilitystrfromc) | library |
| StrHasPrefix | [utility](utility.md#utilitystrhasprefix) | library |
| Stricmp, Strnicmp | [utility](utility.md#utilitystricmp) | library |
| StrSame, StrSameNoCase | [utility](utility.md#utilitystrsame) | library |
| StrSlice | [utility](utility.md#utilitystrslice) | library |
| StrSplit | [utility](utility.md#utilitystrsplit) | library |
| StrToU64 | [utility](utility.md#utilitystrtou64) | library |
| StrTrim | [utility](utility.md#utilitystrtrim) | library |
| TagInArray | [utility](utility.md#utilitytaginarray) | library |
| Text | [gfx](gfx.md#gfxtext) | library, through `IGfx` |
| ToUpper, ToLower | [utility](utility.md#utilitytoupper) | library |
| UnshareArea | [exec](exec.md#execunsharearea) | kernel |
| Wait | [exec](exec.md#execwait) | kernel |
| WaitPort | [msg](msg.md#execwaitport) | library |
| WaitPorts | [msg](msg.md#execwaitports) | library |
| WaitUntil | [exec](exec.md#execwaituntil) | kernel |

## Known problems found while writing these

Each is under BUGS in its entry.
- `FindTask` still returns `Handle`, so its error comes back as a large unsigned value. (`AllocArea` and `CreateTask` now return `int64_t`.)
- `ExitTask`'s code goes nowhere.
- `SignalWaitConn` returns 0 at once for a bad handle, the same as for a closed peer.
- An over-long port or program name (`CreatePort`, `CreateProcess`) gives `ERR_BADADDR` rather than `ERR_BADARG`.
- If `OpenPort`'s or `AcceptConn`'s `arena` pointer stops being writable mid-call, the connection is made but its handle is lost.
- `KLog` drops lines over its rate limit and still returns 0.
- `AckWaitIrq` reports errors only when its mask is 0. With a nonzero mask a refused call returns 0 at once.
- `MapFirmware` leaves what it mapped in place when it fails part way.
- `AllocMsi` makes the sources, and counts the device as given, before it writes `info`; an unwritable `info` loses them.
- `CreateProcessImage` takes the Area before it makes the process, so an `ERR_NOMEM` loses the Area.
- `ReadConsole` polls the UART: there's no receive interrupt, and what's typed while nobody reads can be lost.
