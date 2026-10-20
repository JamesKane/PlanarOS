/*
 * planar/ipc.h -- connections: the arena's layout and messages
 *
 * docs/04-ipc.md and docs/v2-plan.md. OpenPort makes a connection: an
 * arena Area mapped read-write into the client and, once it accepts, the
 * server, at the same address in both. The kernel writes the header and
 * two empty rings; after that it only ever sets ca_Closed. Everything
 * else is the two sides' business, through lib/exec (planar/msg.h).
 *
 * Each ring has one producer and one consumer. A slot holds a message's
 * address. The producer fills slot[head % slots] and then release-stores
 * head + 1; the consumer takes slot[tail % slots] and release-stores
 * tail + 1. A consumer about to sleep sets cr_Waiting, looks once more,
 * and only then waits; a producer that finds cr_Waiting set clears it
 * and calls SignalConn. That is the only kernel call on the way.
 */
#ifndef PLANAR_IPC_H
#define PLANAR_IPC_H

#include <stdint.h>

#include <planar/exec.h>

constexpr uint32_t CONN_MAGIC = 0x4e4e4f43;     /* "CONN" */
constexpr uint32_t CONN_VERSION = 1;

/* ca_Closed: which ends have gone. */
constexpr uint32_t CONNF_CLIENT_GONE = 1u << 0;
constexpr uint32_t CONNF_SERVER_GONE = 1u << 1;

/* Producer and consumer words on separate cache lines. */
struct ConnRing {
    _Atomic uint32_t cr_Head;       /* written by the producer */
    uint32_t cr_Pad0[15];
    _Atomic uint32_t cr_Tail;       /* written by the consumer */
    _Atomic uint32_t cr_Waiting;    /* the consumer is asleep: signal it */
    uint32_t cr_Pad1[14];
    uint64_t cr_Slot[];
};

struct ConnArena {
    uint32_t ca_Magic;
    uint32_t ca_Version;
    uint32_t ca_Slots;              /* per ring, a power of two */
    _Atomic uint32_t ca_Closed;     /* CONNF_*, set by the kernel */
    uint64_t ca_Size;               /* the whole arena, bytes */
    uint64_t ca_Request;            /* offsets from the arena's start */
    uint64_t ca_Reply;
    uint64_t ca_Heap;               /* free space for messages */
    uint64_t ca_HeapSize;
    uint64_t ca_Pad;
};

static_assert(sizeof(struct ConnArena) == 64, "the header is one cache line");
static_assert(sizeof(struct ConnRing) == 128, "a ring's header is two cache lines");

constexpr uint32_t CONN_SLOTS_DEFAULT = 64;
constexpr uint32_t CONN_SLOTS_MAX = 4096;
constexpr uint64_t CONN_HEAP_DEFAULT = 4096;
constexpr uint64_t CONN_HEAP_MAX = 1ULL << 20;

struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
};

/*
 * A message lives in the arena and is passed by its address. A reply
 * goes back on the connection the message came in on, so there is no
 * reply port.
 */
struct Message {
    struct MinNode mn_Node;         /* the owner's; DoMsg borrows it */
    uint32_t mn_Length;             /* the whole message, header included */
    int32_t mn_Error;               /* the replier's; ERR_PORT_DIED if the peer went */
    uint32_t mn_Flags;
    uint32_t mn_Pad;
    void *mn_Payload;               /* must lie where both sides map */
    uint64_t mn_PayloadLen;
};

/* The kernel calls (arch/<isa>/user/syscalls.S). Those that make a
 * handle return it, or a negative error. */
int64_t CreatePort(const char *name, int64_t sigbit);
int64_t DeletePort(Handle port);
int64_t OpenPort(const char *name, int64_t sigbit, uint32_t slots, uint64_t heap,
                 struct ConnArena **arena);
int64_t AcceptConn(Handle port, int64_t sigbit, struct ConnArena **arena);
int64_t SignalConn(Handle conn);
uint64_t SignalWaitConn(Handle conn, uint64_t mask);
int64_t CloseConn(Handle conn);

#endif /* PLANAR_IPC_H */
