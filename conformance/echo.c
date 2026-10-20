/*
 * conformance/echo.c -- a server: adds one to each message and replies
 *
 * Serves the port "echo", up to MAX_CLIENTS connections at once, until
 * it's killed. It sleeps only when every connection is empty, so a busy
 * stream of messages costs it no kernel calls; it wakes the clients it
 * answered as it goes to sleep, in the same kernel call; and it looks
 * for new connections only when the port's signal says one has come. From
 * omega's conformance/echo.c, over connections.
 */
#include "echo.h"

constexpr unsigned MAX_CLIENTS = 8;

int main(void)
{
    int64_t psig = AllocSignal(-1), csig = AllocSignal(-1);
    int64_t port = psig > 0 && csig > 0 ? CreatePort("echo", psig) : -1;
    struct MsgPort clients[MAX_CLIENTS];
    uint64_t got = 1ULL << psig;
    unsigned n = 0;

    if (port < 0) {
        KLog("echo: can't set up the port");
        return 1;
    }
    for (;;) {
        if (got & (1ULL << psig))
            while (n < MAX_CLIENTS && AcceptPort(&clients[n], (Handle)port, csig) == 0)
                n++;
        for (unsigned i = 0; i < n; i++) {
            struct MsgPort *mp = &clients[i];
            struct Message *m;

            while ((m = GetMsg(mp))) {
                ((struct EchoMsg *)m)->em_Value++;
                if (ReplyMsgLater(mp, m) < 0)
                    break;      /* gone, or a client overfilling its replies */
            }
            if (PortGone(mp) || m) {
                ClosePort(mp);
                clients[i--] = clients[--n];
            }
        }
        got = WaitPorts(clients, n, 1ULL << psig | 1ULL << csig);
    }
}
