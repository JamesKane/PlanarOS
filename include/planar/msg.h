/*
 * planar/msg.h -- Exec's messages over connections (lib/exec/msg.c)
 *
 * A MsgPort is one end of a connection, as the program holding it sees
 * it: the arena, which ring is its own to fill and which to drain, and
 * the signal it waits for. ConnectPort makes the client's end and
 * AcceptPort the server's. Requests go out on one ring and replies come
 * back on the other, so PutMsg and ReplyMsg are the same operation from
 * either side. Nothing here traps unless the other side is asleep
 * (planar/ipc.h).
 *
 * Everything that comes off a ring is checked: indices against the ring
 * size, and each message's header against the arena. A connection that
 * fails a check is broken and acts as if the peer had gone. The checks
 * cover only the header: a server copies what it reads from a message
 * before it trusts it.
 *
 * Unlike Exec's, ReplyMsg names the port: the reply goes back on the
 * connection the message came in on.
 */
#ifndef PLANAR_MSG_H
#define PLANAR_MSG_H

#include <planar/ipc.h>

/* Replies DoMsg can set aside while it waits for its own. */
constexpr unsigned MSGPORT_STASH = 32;

struct MsgPort {
    struct ConnArena *mp_Arena;
    struct ConnRing *mp_Out;        /* the ring this end fills */
    struct ConnRing *mp_In;         /* the ring this end drains */
    Handle mp_Conn;
    uint64_t mp_SigMask;            /* what this end waits for */
    uint32_t mp_Slots;
    uint32_t mp_Gone;               /* the ca_Closed bit for the peer */
    uint32_t mp_OutHead;            /* own copies: the peer can scribble the rings */
    uint32_t mp_InTail;
    uint64_t mp_HeapLo, mp_HeapHi;  /* where messages may lie */
    uint64_t mp_HeapNext;           /* AllocMsg's next byte */
    bool mp_Broken;
    bool mp_Owed;                   /* ReplyMsgLater held back a signal */
    uint8_t mp_StashHead, mp_StashCount;
    /* A message DoMsgUntil gave up on: still the server's, until its
     * reply comes (dropped then) or the port goes. */
    struct Message *mp_Lost;
    /* Replies DoMsg set aside, oldest first: in this end's own memory,
     * never linked through messages the peer can write (review H5). */
    struct Message *mp_Stash[MSGPORT_STASH];
};

/* The client's end: a connection to port 'name'. 0 or an error. */
int64_t ConnectPort(struct MsgPort *mp, const char *name, int64_t sigbit, uint32_t slots,
                    uint64_t heap);
/* The same, waiting until 'deadline' for the port to be made (OpenPort's
 * wait, V6 step 7): ERR_TIMEOUT if it isn't by then. */
int64_t ConnectPortUntil(struct MsgPort *mp, const char *name, int64_t sigbit, uint32_t slots,
                         uint64_t heap, uint64_t deadline);

/* The server's end: the oldest connection waiting on 'port'. 0,
 * ERR_NOTFOUND if none waits, or an error (ERR_BADARG for an arena that
 * isn't laid out as the kernel lays them out). */
int64_t AcceptPort(struct MsgPort *mp, Handle port, int64_t sigbit);

void ClosePort(struct MsgPort *mp);

/* Space for a message in the arena's heap, for the client; nullptr when
 * the heap is used up. There is no freeing: messages are reused. */
void *AllocMsg(struct MsgPort *mp, uint64_t size);

/* Send a message, or a reply: 0, ERR_FULL, ERR_PORT_DIED, or
 * ERR_BADADDR for a message outside the arena. */
int64_t PutMsg(struct MsgPort *mp, struct Message *m);
int64_t ReplyMsg(struct MsgPort *mp, struct Message *m);

/* ReplyMsg, except that a signal the reply owes the peer waits for
 * FlushPort or WaitPorts: a server that answers a batch and then sleeps
 * makes one kernel call for both. */
int64_t ReplyMsgLater(struct MsgPort *mp, struct Message *m);
void FlushPort(struct MsgPort *mp);

/* The next message or reply, or nullptr if there's none (yet). Only its
 * header is known to lie in the arena; GetMsgSized() checks that 'size'
 * bytes do, so a server can read and write that much of it without
 * reaching outside (review H4). A message that fails breaks the
 * connection. */
struct Message *GetMsg(struct MsgPort *mp);
struct Message *GetMsgSized(struct MsgPort *mp, uint64_t size);

/* Wait for the next message or reply; nullptr once the peer has gone
 * and nothing is left. */
struct Message *WaitPort(struct MsgPort *mp);

/* For a task waiting on several ports at once: say this end is about to
 * sleep. True if it's empty and the task may Wait; false if something
 * came in meanwhile (or the peer went), and it should look again. */
bool ArmPort(struct MsgPort *mp);

/*
 * For a server: sleep until any of the 'n' ports has something or a
 * signal in 'mask' comes, first sending the signals ReplyMsgLater held
 * back (the last of them in the same kernel call as the sleep, which
 * hands the CPU to that client). Returns the signals that woke it, or 0
 * if something came in before it could sleep: look again.
 */
uint64_t WaitPorts(struct MsgPort *ports, unsigned n, uint64_t mask);

/* The peer has gone, or the connection broke. */
bool PortGone(const struct MsgPort *mp);

/* Send a message and wait for its reply: 0, or ERR_PORT_DIED (and then
 * mn_Error says so too). Other replies arriving first wait for GetMsg,
 * up to MSGPORT_STASH of them; ERR_FULL if there are more. */
int64_t DoMsg(struct MsgPort *mp, struct Message *m);

/*
 * DoMsg with a deadline (V6 step 7): ERR_TIMEOUT if the reply hasn't come
 * by then. The message is still the server's: it's in mp_Lost until the
 * reply comes, which is then dropped, wherever it's taken. Until then
 * every DoMsgUntil on the port is ERR_BUSY, unless it has gone.
 */
int64_t DoMsgUntil(struct MsgPort *mp, struct Message *m, uint64_t deadline);

/*
 * Tell the supervisor, hwdb, that the server at port 'portName' missed a
 * deadline (lib/exec/report.c): it restarts it, once a second at most.
 * 0, or why it couldn't be told. It waits a second at most for hwdb, on
 * signal 63.
 */
int64_t ReportUnresponsive(const char *portName);

#endif /* PLANAR_MSG_H */
