# Autodocs

*Planar Reference Manuals, volume 3.* One entry per call, in the RKM's format. The format is in [STYLE.md](../STYLE.md#autodocs).

| File | Contents |
|---|---|
| [exec.md](exec.md) | The kernel calls (`planar/exec.h`, `planar/ipc.h`) |
| [msg.md](msg.md) | Exec's messages over connections (`planar/msg.h`, `lib/exec`) |

## All calls

| Call | Where | |
|---|---|---|
| AcceptConn | [exec](exec.md#execacceptconn) | kernel |
| AcceptPort | [msg](msg.md#execacceptport) | library |
| AllocArea | [exec](exec.md#execallocarea) | kernel |
| AllocMsg | [msg](msg.md#execallocmsg) | library |
| AllocSignal | [exec](exec.md#execallocsignal) | kernel |
| ArmPort | [msg](msg.md#execarmport) | library |
| CloseConn | [exec](exec.md#execcloseconn) | kernel |
| ClosePort | [msg](msg.md#execcloseport) | library |
| ConnectPort | [msg](msg.md#execconnectport) | library |
| CreatePort | [exec](exec.md#execcreateport) | kernel |
| CreateTask | [exec](exec.md#execcreatetask) | kernel |
| DeletePort | [exec](exec.md#execdeleteport) | kernel |
| DoMsg | [msg](msg.md#execdomsg) | library |
| ExitProcess | [exec](exec.md#execexitprocess) | kernel |
| ExitTask | [exec](exec.md#execexittask) | kernel |
| FindTask | [exec](exec.md#execfindtask) | kernel |
| FlushPort | [msg](msg.md#execflushport) | library |
| FreeArea | [exec](exec.md#execfreearea) | kernel |
| FreeSignal | [exec](exec.md#execfreesignal) | kernel |
| GetMsg | [msg](msg.md#execgetmsg) | library |
| GetMsgSized | [msg](msg.md#execgetmsgsized) | library |
| KLog | [exec](exec.md#execklog) | kernel |
| OpenPort | [exec](exec.md#execopenport) | kernel |
| PortGone | [msg](msg.md#execportgone) | library |
| ProtectArea | [exec](exec.md#execprotectarea) | kernel |
| PutMsg | [msg](msg.md#execputmsg) | library |
| QueryArea | [exec](exec.md#execqueryarea) | kernel |
| ReplyMsg | [msg](msg.md#execreplymsg) | library |
| ReplyMsgLater | [msg](msg.md#execreplymsglater) | library |
| SetSignal | [exec](exec.md#execsetsignal) | kernel |
| ShareArea | [exec](exec.md#execsharearea) | kernel |
| Signal | [exec](exec.md#execsignal) | kernel |
| SignalConn | [exec](exec.md#execsignalconn) | kernel |
| SignalWaitConn | [exec](exec.md#execsignalwaitconn) | kernel |
| UnshareArea | [exec](exec.md#execunsharearea) | kernel |
| Wait | [exec](exec.md#execwait) | kernel |
| WaitPort | [msg](msg.md#execwaitport) | library |
| WaitPorts | [msg](msg.md#execwaitports) | library |

## Known problems found while writing these

Each is under BUGS in its entry.
- `FindTask` still returns `Handle`, so its error comes back as a large unsigned value. (`AllocArea` and `CreateTask` now return `int64_t`.)
- `ExitTask`'s code goes nowhere.
- `SignalWaitConn` returns 0 at once for a bad handle, the same as for a closed peer.
- An over-long port name gives `ERR_BADADDR` rather than `ERR_BADARG`.
- If `OpenPort`'s or `AcceptConn`'s `arena` pointer stops being writable mid-call, the connection is made but its handle is lost.
- `KLog` drops lines over its rate limit and still returns 0, and the console is still polled.
