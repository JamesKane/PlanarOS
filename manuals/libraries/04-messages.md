# 4. Exec: ports, connections and messages

*Checked against V3.*

Programs talk by sending **messages**: structures in shared memory, passed by address and replied to in place, as on the Amiga. What Planar adds is protection. Two programs can exchange messages only over a **connection**, and a connection's memory is mapped into those two programs and no others.

While both sides are busy, sending and replying need no kernel calls at all. The kernel is entered only to wake a side that has gone to sleep.

## Concepts

| Object | Made by | What it is |
|---|---|---|
| **Port** | Server: `CreatePort(name, sigbit)` | A name clients can connect to. It has no queue of messages, only a queue of connections waiting to be accepted. |
| **Connection** | Client: `ConnectPort(&mp, name, …)`. Server: `AcceptPort(&mp, port, …)` | A private channel between one client and one server: an **arena** Area mapped into both, holding a request ring, a reply ring and a heap for messages. |
| **`struct MsgPort`** | Either `…Port` call | One end of a connection, as the program holding it sees it. It lives in the program's own memory. |
| **Message** | Client: `AllocMsg(&mp, size)` | A `struct Message` header followed by the program's own fields, in the arena's heap. |

The flow:
1. The server creates a port with a signal bit. That bit fires when a client connects.
2. A client connects by name. The kernel makes the arena, maps it into the client, queues the connection on the port and signals the server.
3. The server accepts, naming a signal bit of its own for this connection. The kernel maps the same arena into the server, at the same address.
4. The client allocates messages from the arena's heap and sends them with `PutMsg` or `DoMsg`. The server takes them with `GetMsgSized` (or `GetMsg`) and answers with `ReplyMsg`. A reply goes back on the connection the message came in on. There is no reply port.
5. Either side closes with `ClosePort`. If one side ends, by closing, exiting or crashing, the other side's calls fail with `ERR_PORT_DIED`, and `PortGone` says so.

Messages carry their own data in the fields after the header. A message can also point to data elsewhere with `mn_Payload`, but that memory must be mapped by both sides: in V2 that means elsewhere in the same arena.

## Using it

### A message type

Both sides agree on a structure that starts with `struct Message`. From `conformance/echo.h`:

```c
struct EchoMsg {
    struct Message em_Msg;
    uint64_t em_Value;          /* the server adds one */
    uint64_t em_Seq;
};
```

### A client

From `conformance/pingpong.c`, without the timing:

```c
struct MsgPort mp;
struct EchoMsg *m;
int64_t sig = AllocSignal(-1);

if (ConnectPort(&mp, "echo", sig, 0, 0) < 0)        /* default ring and heap */
    return -1;
if (!(m = AllocMsg(&mp, sizeof *m)))
    return -1;
m->em_Msg.mn_Length = sizeof *m;
m->em_Value = 41;
if (DoMsg(&mp, &m->em_Msg) == 0)                    /* send, wait for the reply */
    KLog(m->em_Value == 42 ? "right" : "wrong");
ClosePort(&mp);
```

`DoMsg` is the synchronous form. If the server is asleep, the kernel hands this CPU straight to the server and lends it the client's priority until it next signals or waits, which for a simple server is its reply. On QEMU that happens on about 98% of round trips (`docs/v2-plan.md`).

This is **handoff boosting**, not full priority inheritance ([docs §4.4](../../docs/04-ipc.md)). A server that is already running or ready when the request comes is lent nothing. A server that waits on another server doesn't pass the loan on. And the loan ends at the server's next signal or wait, even if the request isn't answered yet.

For several messages in flight, use `PutMsg` for each and collect the replies with `WaitPort` or `GetMsg`. Replies can come back in any order.

### A server for many clients

From `conformance/echo.c`. The server sleeps only when every connection is empty. It takes new connections only when the port's signal says one has come.

```c
int64_t psig = AllocSignal(-1), csig = AllocSignal(-1);
int64_t port = CreatePort("echo", psig);
struct MsgPort clients[8];
uint64_t got = 1ULL << psig;
unsigned n = 0;

for (;;) {
    if (got & (1ULL << psig))
        while (n < 8 && AcceptPort(&clients[n], (Handle)port, csig) == 0)
            n++;
    for (unsigned i = 0; i < n; i++) {
        struct MsgPort *mp = &clients[i];
        struct Message *m;

        while ((m = GetMsgSized(mp, sizeof(struct EchoMsg)))) {
            ((struct EchoMsg *)m)->em_Value++;
            if (ReplyMsgLater(mp, m) < 0)
                break;
        }
        if (PortGone(mp) || m) {                     /* client gone, or misbehaving */
            ClosePort(mp);
            clients[i--] = clients[--n];
        }
    }
    got = WaitPorts(clients, n, 1ULL << psig | 1ULL << csig);
}
```

Points to notice:
- `GetMsgSized` checks that the whole `EchoMsg` lies in the arena before the server writes `em_Value`. `GetMsg` would check only the header, and a client could send a message whose header fits at the very end of the heap, so that the write lands outside (`conformance/rogue.c`, case 3). A message that fails the check breaks that connection, and the loop closes it.
- All the connections share one signal bit, `csig`. A signal means "look at your connections", not "connection *i* has something".
- `ReplyMsgLater` replies without waking the client yet. `WaitPorts` then wakes every client that needs it on the way to sleep, the last of them in the same kernel call as the sleep.
- `WaitPorts` returns 0 if something arrived while it was getting ready to sleep. The loop simply looks again.
- The server can hold at most 32 connection ends (chapter 1, *Limits*), so it could never serve more than 32 clients at once. Echo stops at 8.

## How it works

You don't need this to use messages, but it explains the costs. The layout is in `planar/ipc.h`.

Each ring has one producer and one consumer. A slot holds a message's address.
- **To send**, the producer writes the address into the next slot and advances `cr_Head`.
- **To sleep**, a consumer sets `cr_Waiting`, checks the ring once more, and only then calls `Wait`.
- **A producer that finds `cr_Waiting` set** clears it and calls `SignalConn`. That is the only kernel call on the path.

A stream of messages to a server that keeps up costs no kernel calls. In V2's test, streaming 20,000 messages took about 24–32 calls per 1,000 messages on each side.

When the server is asleep, `DoMsg` wakes it and hands it the CPU. A round trip with the echo server, both on one CPU, costs about 1,530 cycles (0.8 µs) on the CIX's Cortex-A720, 3,370 (1.9 µs) on its Cortex-A520, and 3,210 (2 µs) on the RV2.

## Rules

- **Fill in `mn_Length`** with the whole message's size, header included.
- **Don't touch a message after sending it** until its reply comes back. Nothing enforces this. Breaking it can only corrupt your own connection, but it will.
- **Take messages at their full size.** `GetMsg` checks only that the header lies in the arena. A server that reads or writes past the header uses `GetMsgSized(mp, size)`, which checks `size` bytes.
- **A server copies what it checks.** The client can still write to the arena after sending. Copy fields out of a message *before* validating them, and use the copies. That includes `mn_Length`.
- **Messages must come from `AllocMsg`**, or at least lie in the arena's heap, 8-byte aligned. `PutMsg` refuses anything else with `ERR_BADADDR`.
- **`AllocMsg` has no free.** Allocate the messages you need once and reuse them. The default heap is 4 KiB, and `ConnectPort` can ask for up to 1 MiB.
- **A full ring returns `ERR_FULL`.** The default ring has 64 slots. Either wait for replies before sending more, or ask for a bigger ring when connecting (up to 4096 slots, a power of two).
- **`DoMsg` sets aside at most 32 other replies.** Replies to earlier `PutMsg`s that arrive while `DoMsg` waits are kept for `GetMsg`, up to `MSGPORT_STASH`. Beyond that `DoMsg` returns `ERR_FULL` with its own message sent but unanswered. Collect replies before calling `DoMsg` with many messages in flight.
- **A port queues at most 32 connections.** Past that, `ConnectPort` fails with `ERR_FULL` until the server accepts some. A process holds at most 32 connection ends, client and server together; past that, `ERR_LIMIT`.
- **A connection belongs to the task that opened or accepted it.** It closes when that task ends, so don't hand a `MsgPort` to another task in the same process and then end the first one.
- **A process can't connect to its own port.** That fails with `ERR_BADARG`.
- **Anything off a ring is checked.** A connection whose peer has written nonsense into the rings, or sent a message that fails the check, is broken: it acts as if the peer had gone (`PortGone` is true). The message that failed is lost.

## Differences from Exec

| Exec | Planar |
|---|---|
| `PutMsg(port, msg)` to any public port | `ConnectPort` first; then `PutMsg(&mp, msg)` on that connection |
| `mn_ReplyPort` says where to reply | No reply port. A reply returns on the connection it came in on, so it can't be forged |
| `WaitPort` waits but leaves the message on the port | `WaitPort` waits **and removes** the message, like `WaitPort` followed by `GetMsg` |
| `ReplyMsg(msg)` | `ReplyMsg(&mp, msg)`: names the connection |
| `FindPort(name)` under `Forbid` | `ConnectPort` by name; the kernel does the lookup |
| One message list per port, of any length | Two fixed-size rings per connection; `ERR_FULL` when one is full |
| No error field | `mn_Error`: `DoMsg` sets `ERR_PORT_DIED` there if the server went |
| Messages anywhere in memory | Messages in the connection's arena |

## Functions used

Library ([msg autodocs](../autodocs/msg.md)):
[AcceptPort](../autodocs/msg.md#execacceptport),
[AllocMsg](../autodocs/msg.md#execallocmsg),
[ArmPort](../autodocs/msg.md#execarmport),
[ClosePort](../autodocs/msg.md#execcloseport),
[ConnectPort](../autodocs/msg.md#execconnectport),
[DoMsg](../autodocs/msg.md#execdomsg),
[FlushPort](../autodocs/msg.md#execflushport),
[GetMsg](../autodocs/msg.md#execgetmsg),
[GetMsgSized](../autodocs/msg.md#execgetmsgsized),
[PortGone](../autodocs/msg.md#execportgone),
[PutMsg](../autodocs/msg.md#execputmsg),
[ReplyMsg](../autodocs/msg.md#execreplymsg),
[ReplyMsgLater](../autodocs/msg.md#execreplymsglater),
[WaitPort](../autodocs/msg.md#execwaitport),
[WaitPorts](../autodocs/msg.md#execwaitports).

Kernel calls underneath ([exec autodocs](../autodocs/exec.md)):
[CreatePort](../autodocs/exec.md#execcreateport),
[DeletePort](../autodocs/exec.md#execdeleteport),
[OpenPort](../autodocs/exec.md#execopenport),
[AcceptConn](../autodocs/exec.md#execacceptconn),
[SignalConn](../autodocs/exec.md#execsignalconn),
[SignalWaitConn](../autodocs/exec.md#execsignalwaitconn),
[CloseConn](../autodocs/exec.md#execcloseconn).
