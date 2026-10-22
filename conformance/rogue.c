/*
 * conformance/rogue.c -- a client that breaks the rules
 *
 * Connects to echo and then, with argument 0, puts a pointer outside
 * the arena on the ring; with 1, moves the ring's head far past its
 * tail; with 2, sends a message and scribbles over the rings while the
 * server works; with 3, sends a message whose header fits in the heap
 * but whose body runs past its end. The server must survive, drop this connection, and go
 * on serving others. Exits 0 once the server has dropped it.
 */
#include <stdatomic.h>

#include <planar/msg.h>
#include <planar/syscalls.h>

int main(uint64_t how)
{
    int64_t sig = AllocSignal(-1), err;
    struct MsgPort mp;
    struct ConnRing *r;

    /* The server may not be up yet: wait for its port. */
    err = ConnectPortUntil(&mp, "echo", sig, 0, 0, CurrentTime() + 10000000000ULL);
    if (err < 0)
        return 1;
    r = mp.mp_Out;
    if (how == 0) {
        r->cr_Slot[mp.mp_OutHead % mp.mp_Slots] = 0x10;
        atomic_store(&r->cr_Head, mp.mp_OutHead + 1);
    } else if (how == 1) {
        atomic_store(&r->cr_Head, mp.mp_OutHead + 1000);
    } else if (how == 3) {
        r->cr_Slot[mp.mp_OutHead % mp.mp_Slots] = mp.mp_HeapHi - sizeof(struct Message);
        atomic_store(&r->cr_Head, mp.mp_OutHead + 1);
    } else {
        for (unsigned i = 0; i < mp.mp_Slots; i++)
            r->cr_Slot[i] = (uint64_t)mp.mp_Arena + 8;  /* in the arena, not the heap */
        atomic_store(&r->cr_Head, mp.mp_OutHead + 3);
    }
    SignalConn(mp.mp_Conn);
    while (!(atomic_load(&mp.mp_Arena->ca_Closed) & CONNF_SERVER_GONE))
        Wait(1ULL << sig);
    return 0;
}
