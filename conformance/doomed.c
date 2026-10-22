/*
 * conformance/doomed.c -- a client whose server dies while it waits
 *
 * Sends one message to "sink" with DoMsg, which never answers; the
 * kernel kills the server. DoMsg must come back with ERR_PORT_DIED, say
 * so in the message too, and the connection must stay dead. Exits 0 if
 * so, otherwise with the number of the check that failed.
 */
#include <planar/msg.h>
#include <planar/syscalls.h>

int main(void)
{
    int64_t sig = AllocSignal(-1), err;
    struct Message *m;
    struct MsgPort mp;

    /* The server may not be up yet: wait for its port. */
    err = ConnectPortUntil(&mp, "sink", sig, 0, 0, CurrentTime() + 10000000000ULL);
    if (err < 0 || !(m = AllocMsg(&mp, sizeof *m)))
        return 1;
    m->mn_Length = sizeof *m;
    m->mn_Error = 0;
    if (DoMsg(&mp, m) != ERR_PORT_DIED)
        return 2;
    if (m->mn_Error != ERR_PORT_DIED || !PortGone(&mp))
        return 3;
    if (PutMsg(&mp, m) != ERR_PORT_DIED || DoMsg(&mp, m) != ERR_PORT_DIED)
        return 4;
    ClosePort(&mp);
    return 0;
}
