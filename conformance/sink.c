/*
 * conformance/sink.c -- a server that takes messages and never answers
 *
 * Serves one connection on the port "sink", so the kernel can kill it
 * while its client waits in DoMsg. Exits 0 if the client goes first.
 */
#include <planar/msg.h>
#include <planar/syscalls.h>

int main(void)
{
    int64_t psig = AllocSignal(-1), csig = AllocSignal(-1), port, err;
    struct MsgPort mp;

    if ((port = CreatePort("sink", psig)) < 0)
        return 1;
    while ((err = AcceptPort(&mp, (Handle)port, csig)) == ERR_NOTFOUND)
        Wait(1ULL << psig);
    if (err < 0)
        return 2;
    while (WaitPort(&mp))
        ;
    return 0;
}
