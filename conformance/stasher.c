/*
 * conformance/stasher.c -- DoMsg with other replies arriving first
 *
 * Sends two messages with PutMsg and a third with DoMsg to "stash".
 * DoMsg must return with the third's reply, setting the first two aside;
 * GetMsg must then give exactly those two, in order, however the server
 * tampers with them.
 *
 * Then fills the set-aside list: 33 messages answered while this end
 * doesn't look, the server asleep, and a DoMsg that must say ERR_FULL
 * and still wake the server, since sending found it asleep. The last
 * message's reply then has to arrive. Exits 0 if so, otherwise the
 * number of the check that failed.
 */
#include <stdatomic.h>

#include <planar/msg.h>
#include <planar/syscalls.h>

int main(void)
{
    int64_t sig = AllocSignal(-1), err;
    struct Message *m[3];
    struct MsgPort mp;

    while ((err = ConnectPort(&mp, "stash", sig, 0, 0)) == ERR_NOTFOUND)
        ;
    if (err < 0)
        return 1;
    for (unsigned i = 0; i < 3; i++) {
        if (!(m[i] = AllocMsg(&mp, sizeof *m[i])))
            return 2;
        m[i]->mn_Length = sizeof *m[i];
    }
    if (PutMsg(&mp, m[0]) || PutMsg(&mp, m[1]))
        return 3;
    if (DoMsg(&mp, m[2]) != 0)
        return 4;
    if (GetMsg(&mp) != m[0])
        return 5;
    if (GetMsg(&mp) != m[1])
        return 6;
    if (GetMsg(&mp) != nullptr)
        return 7;

    struct Message *f[MSGPORT_STASH + 2];

    for (unsigned i = 0; i < MSGPORT_STASH + 2; i++) {
        if (!(f[i] = AllocMsg(&mp, sizeof *f[i])))
            return 8;
        f[i]->mn_Length = sizeof *f[i];
    }
    for (unsigned i = 0; i < MSGPORT_STASH + 1; i++)
        if (PutMsg(&mp, f[i]))
            return 9;
    /* Every reply is in once the server sleeps for the last message;
     * a waiting flag still set after a while means it really does. */
    for (unsigned settled = 0; settled < 3;) {
        settled = atomic_load_explicit(&mp.mp_Out->cr_Waiting, memory_order_acquire)
                      ? settled + 1 : 0;
        for (volatile unsigned spin = 0; spin < 100000; spin++)
            ;
    }
    if (DoMsg(&mp, f[MSGPORT_STASH + 1]) != ERR_FULL)
        return 10;
    for (unsigned i = 0; i < MSGPORT_STASH + 1; i++)
        if (GetMsg(&mp) != f[i])
            return 11;
    if (WaitPort(&mp) != f[MSGPORT_STASH + 1])  /* hangs if never woken */
        return 12;
    ClosePort(&mp);
    return 0;
}
