# exec: messages (`lib/exec`)

*Checked against V5.*

Exec's message-port calls, built on connections by `lib/exec/msg.c`, which is linked into every user program. The header is `planar/msg.h`. These are library functions: they trap only where an entry says so. The concepts are in [Libraries ch. 4](../libraries/04-messages.md).

A `struct MsgPort` is one end of one connection, in the caller's memory. Don't change its fields.

---

## exec/AcceptPort

**NAME**
AcceptPort -- take the server's end of a waiting connection

**SYNOPSIS**
```c
#include <planar/msg.h>
int64_t AcceptPort(struct MsgPort *mp, Handle port, int64_t sigbit);
```

**FUNCTION**
`AcceptConn` on `port`, then checks the arena is laid out as the kernel lays them out, and sets up `mp` as the server's end.

**INPUTS**
- `mp` -- filled in.
- `port` -- from `CreatePort`.
- `sigbit` -- the signal bit this end waits on. Several connections may share one.

**RESULT**
0, or:
- `ERR_NOTFOUND` -- no connection is waiting. The usual way to stop accepting.
- `ERR_BADARG` -- `sigbit` out of range, or the arena isn't laid out as the kernel lays them out (the connection is closed again).
- Any other error from `AcceptConn`, notably `ERR_LIMIT` once the process holds 32 connection ends.

**BUGS**
None known.

**SEE ALSO**
[AcceptConn](exec.md#execacceptconn), [ConnectPort](#execconnectport), [WaitPorts](#execwaitports)

---

## exec/AllocMsg

**NAME**
AllocMsg -- space for a message in a connection's arena

**SYNOPSIS**
```c
#include <planar/msg.h>
void *AllocMsg(struct MsgPort *mp, uint64_t size);
```

**FUNCTION**
Returns `size` bytes from the arena's heap, 16-byte aligned. Doesn't trap.

**INPUTS**
- `mp` -- the client's end.
- `size` -- at least `sizeof(struct Message)`.

**RESULT**
The address, or `nullptr` if `size` is too small or the heap is used up.

**NOTES**
- There is no `FreeMsg`. Allocate what you need once and reuse it.
- The memory isn't cleared after first use. Set `mn_Length` and your own fields.
- Only the client allocates. The server replies in the message it was sent.

**BUGS**
None known.

**SEE ALSO**
[PutMsg](#execputmsg), [DoMsg](#execdomsg)

---

## exec/ArmPort

**NAME**
ArmPort -- tell the other end this end is about to sleep

**SYNOPSIS**
```c
#include <planar/msg.h>
bool ArmPort(struct MsgPort *mp);
```

**FUNCTION**
Sets this end's `cr_Waiting` flag, so that the other end's next send will signal, then looks once more. Doesn't trap.

For a task waiting on several ports and other signals at once:

```c
bool sleep = true;

for (unsigned i = 0; i < n && sleep; i++)
    sleep = ArmPort(&ports[i]);
if (sleep)
    Wait(mask);
/* then GetMsg on every port */
```

**RESULT**
True if nothing is waiting and the caller may `Wait`. False if a message or reply has come in, or the peer has gone: look again, don't sleep.

**NOTES**
`WaitPort` and `WaitPorts` call this for you. A server should normally use `WaitPorts`.

**BUGS**
None known.

**SEE ALSO**
[WaitPorts](#execwaitports), [WaitPort](#execwaitport)

---

## exec/ClosePort

**NAME**
ClosePort -- close this end of a connection

**SYNOPSIS**
```c
#include <planar/msg.h>
void ClosePort(struct MsgPort *mp);
```

**FUNCTION**
`CloseConn` on this end, and marks `mp` broken so later calls on it fail with `ERR_PORT_DIED`. Messages in the arena can't be used afterwards. A kernel call.

**BUGS**
None known.

**SEE ALSO**
[CloseConn](exec.md#execcloseconn), [PortGone](#execportgone)

---

## exec/ConnectPort

**NAME**
ConnectPort -- connect to a named port

**SYNOPSIS**
```c
#include <planar/msg.h>
int64_t ConnectPort(struct MsgPort *mp, const char *name, int64_t sigbit, uint32_t slots,
                    uint64_t heap);
```

**FUNCTION**
`OpenPort`, then sets up `mp` as the client's end. Messages can be sent at once, before the server accepts.

**INPUTS**
- `mp` -- filled in.
- `name`, `sigbit`, `slots`, `heap` -- as for `OpenPort`. 0 for `slots` or `heap` takes the default.

**RESULT**
0, or any error from `OpenPort`, notably:
- `ERR_NOTFOUND` -- the server hasn't created the port yet.
- `ERR_FULL` -- 32 connections already wait on the port: the server isn't accepting.
- `ERR_LIMIT` -- the process already holds 32 connection ends, or 256 handles.

**NOTES**
The conformance clients retry on `ERR_NOTFOUND` while their server starts. A name service to wait on is **planned**.

**BUGS**
None known.

**SEE ALSO**
[OpenPort](exec.md#execopenport), [AcceptPort](#execacceptport), [ClosePort](#execcloseport)

---

## exec/DoMsg

**NAME**
DoMsg -- send a message and wait for its reply

**SYNOPSIS**
```c
#include <planar/msg.h>
int64_t DoMsg(struct MsgPort *mp, struct Message *m);
```

**FUNCTION**
Sends `m` and waits until it comes back. If the server is asleep, wakes it and waits in one kernel call (`SignalWaitConn`): the server runs next on this CPU, at no less than the caller's priority, until it next signals or waits (handoff boosting; see [SignalWaitConn](exec.md#execsignalwaitconn)). If the server is awake, waiting costs no kernel call unless the reply is slow.

Replies to other messages that arrive first are set aside for `GetMsg`, oldest first, up to `MSGPORT_STASH` (32) of them. They are kept in the `MsgPort`, in the caller's own memory, never linked through the arena.

**INPUTS**
- `mp` -- the client's end.
- `m` -- a message in the arena, with `mn_Length` set.

**RESULT**
0 once replied, or:
- `ERR_PORT_DIED` -- the server went first, or the connection broke. If that happened while `DoMsg` waited, `m->mn_Error` is set to it too; if it had already happened, nothing was sent and `mn_Error` is left alone.
- `ERR_FULL` -- either the request ring is full, and nothing was sent; or `m` was sent but 32 other replies are already set aside and more are waiting. Then `m`'s reply is still to come: take the set-aside replies with `GetMsg` and collect it the same way. `m` reaches the server even if it was asleep: `DoMsg` wakes it before returning (`conformance/stasher.c`).
- `ERR_BADADDR` -- `m` isn't in the arena's heap. Nothing was sent.

**NOTES**
- A server that replied just before it went still counts as a reply.
- Whatever the server put in `mn_Error` is left there. 0 from `DoMsg` means "it came back", not "the server succeeded".
- To tell the two `ERR_FULL`s apart, don't mix `DoMsg` with more than 32 replies outstanding from `PutMsg`.
- Replies are checked as for `GetMsg`, header only. A reply that fails the check breaks the connection.

**BUGS**
None known.

**SEE ALSO**
[PutMsg](#execputmsg), [SignalWaitConn](exec.md#execsignalwaitconn), [Libraries ch. 4](../libraries/04-messages.md)

---

## exec/FlushPort

**NAME**
FlushPort -- send the signal `ReplyMsgLater` held back

**SYNOPSIS**
```c
#include <planar/msg.h>
void FlushPort(struct MsgPort *mp);
```

**FUNCTION**
If a `ReplyMsgLater` on this end found the client asleep, signals it now with `SignalConn`. Otherwise does nothing and doesn't trap.

**Built, untested** directly: `WaitPorts` calls it and is tested.

**NOTES**
A server that doesn't go to sleep with `WaitPorts` must call this after a batch of `ReplyMsgLater`, or the client may sleep for ever.

**BUGS**
None known.

**SEE ALSO**
[ReplyMsgLater](#execreplymsglater), [WaitPorts](#execwaitports)

---

## exec/GetMsg

**NAME**
GetMsg -- take the next message or reply

**SYNOPSIS**
```c
#include <planar/msg.h>
struct Message *GetMsg(struct MsgPort *mp);
```

**FUNCTION**
Returns the next message (on the server's end) or reply (on the client's), or `nullptr` if there's none. Replies `DoMsg` set aside come first. Doesn't trap.

`GetMsg(mp)` is `GetMsgSized(mp, sizeof(struct Message))`.

**RESULT**
The message, or `nullptr`.

**NOTES**
- The library has checked only that the header lies in the arena's heap and is 8-byte aligned. A server that reads or writes past the header uses `GetMsgSized` instead.
- A server copies the fields it needs before trusting them.
- `nullptr` may also mean the peer wrote nonsense into the ring, or sent a message that failed the check. The connection is then broken and `PortGone` is true.

**BUGS**
None known.

**SEE ALSO**
[GetMsgSized](#execgetmsgsized), [WaitPort](#execwaitport), [PortGone](#execportgone)

---

## exec/GetMsgSized

**NAME**
GetMsgSized -- take the next message, checking its size

**SYNOPSIS**
```c
#include <planar/msg.h>
struct Message *GetMsgSized(struct MsgPort *mp, uint64_t size);
```

**FUNCTION**
`GetMsg`, except that the library checks that `size` bytes of the message, not just its header, lie in the arena's heap. A server can then read and write that much of it without reaching outside the arena. Doesn't trap.

For a server whose messages are all one type:

```c
while ((m = GetMsgSized(mp, sizeof(struct EchoMsg))))
    ((struct EchoMsg *)m)->em_Value++;
```

**INPUTS**
- `mp` -- either end.
- `size` -- the bytes the caller will touch. Anything less than `sizeof(struct Message)` is taken as that.

**RESULT**
The message, or `nullptr` if there's none, or if it failed the check.

**NOTES**
- A message that fails the check has been taken off the ring and is lost. The connection is broken: `PortGone` is true, and later calls on `mp` fail. A server closes it and goes on serving others, as `conformance/echo.c` does.
- The check is against `size`, not against `mn_Length`, which the peer could change after the check. Check `mn_Length` too if the message's size varies, on a copy.
- Replies `DoMsg` set aside are checked against `size` as they come out.

**BUGS**
None known.

**SEE ALSO**
[GetMsg](#execgetmsg), [PortGone](#execportgone), [Libraries ch. 4](../libraries/04-messages.md)

---

## exec/PortGone

**NAME**
PortGone -- has the other end gone?

**SYNOPSIS**
```c
#include <planar/msg.h>
bool PortGone(const struct MsgPort *mp);
```

**FUNCTION**
True if the other end has closed (by `ClosePort`, by its task ending or by its process dying), if this end was closed, or if the connection broke: a ring held nonsense, or a message failed `GetMsg`'s or `GetMsgSized`'s check. Doesn't trap.

**NOTES**
Messages already in the ring can still be taken with `GetMsg` after the peer has gone.

**BUGS**
None known.

**SEE ALSO**
[ClosePort](#execcloseport)

---

## exec/PutMsg

**NAME**
PutMsg -- send a message

**SYNOPSIS**
```c
#include <planar/msg.h>
int64_t PutMsg(struct MsgPort *mp, struct Message *m);
```

**FUNCTION**
Puts `m` on this end's outgoing ring. If the other end is asleep, wakes it with `SignalConn`. Otherwise doesn't trap.

From the client's end this sends a request. From the server's it sends a reply: `ReplyMsg` is the same call.

**INPUTS**
- `mp` -- either end.
- `m` -- a message in the arena's heap.

**RESULT**
0, or:
- `ERR_FULL` -- the ring is full. Try again after taking replies.
- `ERR_PORT_DIED` -- the other end has gone, or the connection broke.
- `ERR_BADADDR` -- `m` isn't 8-byte aligned in the arena's heap.

**NOTES**
Don't touch `m` until its reply comes back.

**BUGS**
None known.

**SEE ALSO**
[DoMsg](#execdomsg), [GetMsg](#execgetmsg), [WaitPort](#execwaitport)

---

## exec/ReplyMsg

**NAME**
ReplyMsg -- reply to a message

**SYNOPSIS**
```c
#include <planar/msg.h>
int64_t ReplyMsg(struct MsgPort *mp, struct Message *m);
```

**FUNCTION**
Sends `m` back on the connection it came in on. Exactly `PutMsg` on the server's end.

**Built, untested** directly: it is `PutMsg`, which is tested.

**RESULT**
As for `PutMsg`.

**NOTES**
**Differs from Exec**: names the port, because there's no `mn_ReplyPort`.

**BUGS**
None known.

**SEE ALSO**
[ReplyMsgLater](#execreplymsglater), [PutMsg](#execputmsg)

---

## exec/ReplyMsgLater

**NAME**
ReplyMsgLater -- reply, and wake the client later

**SYNOPSIS**
```c
#include <planar/msg.h>
int64_t ReplyMsgLater(struct MsgPort *mp, struct Message *m);
```

**FUNCTION**
`ReplyMsg`, except that if the client is asleep the signal waits for `FlushPort` or `WaitPorts`. A server that answers a batch and then sleeps makes one kernel call for all of it. Never traps.

**RESULT**
As for `PutMsg`.

**NOTES**
A client may stop sending when its replies stop being collected, so a server that gets `ERR_FULL` here is facing a client that isn't reading its replies. `conformance/echo.c` closes such a connection.

**BUGS**
None known.

**SEE ALSO**
[FlushPort](#execflushport), [WaitPorts](#execwaitports)

---

## exec/WaitPort

**NAME**
WaitPort -- wait for and take the next message or reply

**SYNOPSIS**
```c
#include <planar/msg.h>
struct Message *WaitPort(struct MsgPort *mp);
```

**FUNCTION**
`GetMsg`, sleeping on this end's signal until something comes.

**RESULT**
The message, or `nullptr` once the peer has gone and nothing is left.

**NOTES**
**Differs from Exec**: Exec's `WaitPort` leaves the message on the port. This one removes it.

**BUGS**
None known.

**SEE ALSO**
[GetMsg](#execgetmsg), [WaitPorts](#execwaitports)

---

## exec/WaitPorts

**NAME**
WaitPorts -- a server's sleep over many connections

**SYNOPSIS**
```c
#include <planar/msg.h>
uint64_t WaitPorts(struct MsgPort *ports, unsigned n, uint64_t mask);
```

**FUNCTION**
Arms each of the `n` ports, sends the signals `ReplyMsgLater` held back, and sleeps until a signal in `mask` comes. The last held-back signal goes in the same kernel call as the sleep (`SignalWaitConn`), which hands the CPU to that client.

**INPUTS**
- `ports` -- an array of server ends.
- `n` -- how many. 0 is allowed: then it's `Wait(mask)`.
- `mask` -- every signal that should wake the server, including the ports' own signal bits and the port's connect signal.

**RESULT**
The signals that woke it, or 0 if something came in before it could sleep. Either way, look at every port again.

**BUGS**
None known.

**SEE ALSO**
[ArmPort](#execarmport), [ReplyMsgLater](#execreplymsglater), [Libraries ch. 4](../libraries/04-messages.md#a-server-for-many-clients)
