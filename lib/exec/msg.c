/*
 * lib/exec/msg.c -- Exec's messages over connections (planar/msg.h)
 *
 * The ring protocol is in planar/ipc.h. This end keeps its own copies of
 * the indices it writes and reads only the peer's from the arena, so a
 * peer scribbling on the rings can break the connection but never send
 * this end outside it.
 */
#include <stdatomic.h>

#include <planar/msg.h>
#include <planar/syscalls.h>

static uint64_t ring_bytes(uint32_t slots)
{
    return (sizeof(struct ConnRing) + slots * sizeof(uint64_t) + 63) & ~63ULL;
}

/* Check the arena is laid out as the kernel lays them out, and set up
 * this end from it. 'client' says which end. */
static int64_t attach(struct MsgPort *mp, Handle conn, struct ConnArena *a, int64_t sigbit,
                      bool client)
{
    volatile struct ConnArena *v = a;
    uint32_t slots = v->ca_Slots;
    uint64_t request = v->ca_Request, reply = v->ca_Reply, heap = v->ca_Heap;
    uint64_t size = v->ca_Size, heap_size = v->ca_HeapSize;
    struct AreaInfo info;

    if (v->ca_Magic != CONN_MAGIC || v->ca_Version != CONN_VERSION || slots < 2 ||
        slots > CONN_SLOTS_MAX || (slots & (slots - 1)) || request != sizeof *a ||
        reply != request + ring_bytes(slots) || heap != reply + ring_bytes(slots) ||
        heap + heap_size != size || QueryArea(a, &info) != 0 ||
        info.ai_Address != (void *)a || info.ai_Size != size)
        return ERR_BADARG;
    *mp = (struct MsgPort){
        .mp_Arena = a, .mp_Conn = conn, .mp_SigMask = 1ULL << sigbit, .mp_Slots = slots,
        .mp_Gone = client ? CONNF_SERVER_GONE : CONNF_CLIENT_GONE,
        .mp_HeapLo = (uint64_t)a + heap, .mp_HeapHi = (uint64_t)a + size,
        .mp_HeapNext = (uint64_t)a + heap,
    };
    mp->mp_Out = (struct ConnRing *)((char *)a + (client ? request : reply));
    mp->mp_In = (struct ConnRing *)((char *)a + (client ? reply : request));
    /* Pick up where the rings are (a client may have sent already). */
    mp->mp_OutHead = atomic_load_explicit(&mp->mp_Out->cr_Head, memory_order_relaxed);
    mp->mp_InTail = atomic_load_explicit(&mp->mp_In->cr_Tail, memory_order_relaxed);
    return 0;
}

int64_t ConnectPort(struct MsgPort *mp, const char *name, int64_t sigbit, uint32_t slots,
                    uint64_t heap)
{
    struct ConnArena *a;
    int64_t h = OpenPort(name, sigbit, slots, heap, &a), err;

    if (h < 0)
        return h;
    if ((err = attach(mp, (Handle)h, a, sigbit, true)) < 0)
        CloseConn((Handle)h);
    return err;
}

int64_t AcceptPort(struct MsgPort *mp, Handle port, int64_t sigbit)
{
    struct ConnArena *a;
    int64_t h = AcceptConn(port, sigbit, &a), err;

    if (h < 0)
        return h;
    if ((err = attach(mp, (Handle)h, a, sigbit, false)) < 0)
        CloseConn((Handle)h);
    return err;
}

void ClosePort(struct MsgPort *mp)
{
    CloseConn(mp->mp_Conn);
    mp->mp_Broken = true;
}

bool PortGone(const struct MsgPort *mp)
{
    return mp->mp_Broken ||
           (atomic_load_explicit(&mp->mp_Arena->ca_Closed, memory_order_acquire) & mp->mp_Gone);
}

void *AllocMsg(struct MsgPort *mp, uint64_t size)
{
    uint64_t at = (mp->mp_HeapNext + 15) & ~15ULL;

    if (size < sizeof(struct Message) || size > mp->mp_HeapHi - at)
        return nullptr;
    mp->mp_HeapNext = at + size;
    return (void *)at;
}

/* Do 'size' bytes at 'm' lie in the heap? */
static bool in_heap(const struct MsgPort *mp, uint64_t m, uint64_t size)
{
    return !(m & 7) && m >= mp->mp_HeapLo && m < mp->mp_HeapHi && mp->mp_HeapHi - m >= size;
}

/* --- The rings. --- */

/* Fill the next slot; *asleep says whether the peer must be signalled. */
static int64_t put(struct MsgPort *mp, struct Message *m, bool *asleep)
{
    struct ConnRing *r = mp->mp_Out;
    uint32_t head = mp->mp_OutHead;
    uint32_t tail = atomic_load_explicit(&r->cr_Tail, memory_order_acquire);

    *asleep = false;
    if (PortGone(mp))
        return ERR_PORT_DIED;
    if (!in_heap(mp, (uint64_t)m, sizeof *m))
        return ERR_BADADDR;
    if (head - tail > mp->mp_Slots) {
        mp->mp_Broken = true;
        return ERR_PORT_DIED;
    }
    if (head - tail == mp->mp_Slots)
        return ERR_FULL;
    r->cr_Slot[head & (mp->mp_Slots - 1)] = (uint64_t)m;
    mp->mp_OutHead = ++head;
    atomic_store_explicit(&r->cr_Head, head, memory_order_release);
    /* Against the consumer's: set cr_Waiting, then look at cr_Head. */
    atomic_thread_fence(memory_order_seq_cst);
    *asleep = atomic_load_explicit(&r->cr_Waiting, memory_order_relaxed) &&
              atomic_exchange_explicit(&r->cr_Waiting, 0, memory_order_relaxed);
    return 0;
}

/* Is anything waiting in this end's ring? */
static bool pending(struct MsgPort *mp)
{
    return atomic_load_explicit(&mp->mp_In->cr_Head, memory_order_acquire) != mp->mp_InTail;
}

/* Take the next message from the ring, or nullptr; 'size' bytes of it
 * must lie in the heap. */
static struct Message *take(struct MsgPort *mp, uint64_t size)
{
    struct ConnRing *r = mp->mp_In;
    uint32_t tail = mp->mp_InTail;
    uint32_t head = atomic_load_explicit(&r->cr_Head, memory_order_acquire);
    uint64_t m;

    if (mp->mp_Broken || head == tail)
        return nullptr;
    if (head - tail > mp->mp_Slots) {
        mp->mp_Broken = true;
        return nullptr;
    }
    m = ((volatile uint64_t *)r->cr_Slot)[tail & (mp->mp_Slots - 1)];
    mp->mp_InTail = ++tail;
    atomic_store_explicit(&r->cr_Tail, tail, memory_order_release);
    if (!in_heap(mp, m, size)) {
        mp->mp_Broken = true;
        return nullptr;
    }
    return (struct Message *)m;
}

bool ArmPort(struct MsgPort *mp)
{
    atomic_store_explicit(&mp->mp_In->cr_Waiting, 1, memory_order_relaxed);
    /* Against the producer's: fill the slot, then look at cr_Waiting. */
    atomic_thread_fence(memory_order_seq_cst);
    if (mp->mp_StashCount || pending(mp) || PortGone(mp)) {
        atomic_store_explicit(&mp->mp_In->cr_Waiting, 0, memory_order_relaxed);
        return false;
    }
    return true;
}

/* --- Exec's calls. --- */

int64_t PutMsg(struct MsgPort *mp, struct Message *m)
{
    bool asleep;
    int64_t err = put(mp, m, &asleep);

    if (asleep)
        SignalConn(mp->mp_Conn);
    return err;
}

int64_t ReplyMsg(struct MsgPort *mp, struct Message *m)
{
    return PutMsg(mp, m);
}

int64_t ReplyMsgLater(struct MsgPort *mp, struct Message *m)
{
    bool asleep;
    int64_t err = put(mp, m, &asleep);

    mp->mp_Owed |= asleep;
    return err;
}

void FlushPort(struct MsgPort *mp)
{
    if (mp->mp_Owed) {
        mp->mp_Owed = false;
        SignalConn(mp->mp_Conn);
    }
}

uint64_t WaitPorts(struct MsgPort *ports, unsigned n, uint64_t mask)
{
    struct MsgPort *last = nullptr;
    bool sleep = true;

    for (unsigned i = 0; i < n && sleep; i++)
        sleep = ArmPort(&ports[i]);
    for (unsigned i = 0; i < n; i++) {
        if (!ports[i].mp_Owed)
            continue;
        if (last)
            FlushPort(last);
        last = &ports[i];
    }
    if (!sleep) {
        if (last)
            FlushPort(last);
        return 0;
    }
    if (last) {
        last->mp_Owed = false;
        return SignalWaitConn(last->mp_Conn, mask);
    }
    return Wait(mask);
}

/* Replies set aside come first; each was checked as a whole message of
 * its sender's (this end's own, so its size is the caller's to know). */
struct Message *GetMsgSized(struct MsgPort *mp, uint64_t size)
{
    if (mp->mp_StashCount) {
        struct Message *m = mp->mp_Stash[mp->mp_StashHead];

        mp->mp_StashHead = (uint8_t)((mp->mp_StashHead + 1) % MSGPORT_STASH);
        mp->mp_StashCount--;
        if (in_heap(mp, (uint64_t)m, size))
            return m;
        mp->mp_Broken = true;
        return nullptr;
    }
    return take(mp, size < sizeof(struct Message) ? sizeof(struct Message) : size);
}

struct Message *GetMsg(struct MsgPort *mp)
{
    return GetMsgSized(mp, sizeof(struct Message));
}

struct Message *WaitPort(struct MsgPort *mp)
{
    struct Message *m;

    for (;;) {
        if ((m = GetMsg(mp)))
            return m;
        if (PortGone(mp))
            return nullptr;
        if (ArmPort(mp))
            Wait(mp->mp_SigMask);
    }
}

/* Set a reply aside for GetMsg; the caller made sure there's room. */
static void stash(struct MsgPort *mp, struct Message *m)
{
    mp->mp_Stash[(mp->mp_StashHead + mp->mp_StashCount) % MSGPORT_STASH] = m;
    mp->mp_StashCount++;
}

int64_t DoMsg(struct MsgPort *mp, struct Message *m)
{
    bool asleep;
    struct Message *r;
    int64_t err = put(mp, m, &asleep);

    if (err)
        return err;
    for (;;) {
        /* Room first: a reply taken must never be dropped. With none,
         * this one's reply is still to come: the caller GetMsgs for it. */
        while (mp->mp_StashCount < MSGPORT_STASH && (r = take(mp, sizeof *r))) {
            if (r == m)
                goto replied;
            stash(mp, r);
        }
        if (mp->mp_StashCount == MSGPORT_STASH && pending(mp)) {
            /* put() cleared cr_Waiting: the wake is ours to send. */
            if (asleep)
                SignalConn(mp->mp_Conn);
            return ERR_FULL;
        }
        if (PortGone(mp))
            break;
        atomic_store_explicit(&mp->mp_In->cr_Waiting, 1, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        if (pending(mp) || PortGone(mp)) {
            atomic_store_explicit(&mp->mp_In->cr_Waiting, 0, memory_order_relaxed);
        } else if (asleep) {
            /* Wake the server and sleep in one call: the kernel hands
             * this CPU straight to it. */
            SignalWaitConn(mp->mp_Conn, mp->mp_SigMask);
            asleep = false;
            continue;
        } else {
            Wait(mp->mp_SigMask);
            continue;
        }
        if (asleep) {
            SignalConn(mp->mp_Conn);
            asleep = false;
        }
    }
    /* The server went; it may have answered on its way. */
    while (mp->mp_StashCount < MSGPORT_STASH && (r = take(mp, sizeof *r))) {
        if (r == m)
            goto replied;
        stash(mp, r);
    }
    m->mn_Error = ERR_PORT_DIED;
    return ERR_PORT_DIED;
replied:
    /* Answered, so awake after all: it found the message on its last
     * look before sleeping. No signal owed. */
    return 0;
}
