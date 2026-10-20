/*
 * conformance/stasher.c -- DoMsg with other replies arriving first
 *
 * Sends two messages with PutMsg and a third with DoMsg to "stash".
 * DoMsg must return with the third's reply, setting the first two aside;
 * GetMsg must then give exactly those two, in order, however the server
 * tampers with them. Exits 0 if so, otherwise the number of the check
 * that failed.
 */
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
    ClosePort(&mp);
    return 0;
}
