/*
 * conformance/stashserver.c -- a server that tampers with replies
 *
 * Serves one client on the port "stash": takes its three messages,
 * answers them in order, and all the while keeps writing the first
 * one's mn_Node, the link a careless client would keep its set-aside
 * replies in (docs/10-code-review.md H5). Exits 0 once the client goes.
 */
#include <planar/msg.h>
#include <planar/syscalls.h>

static void tamper(struct Message *m, unsigned rounds)
{
    for (unsigned i = 0; i < rounds; i++) {
        ((volatile struct MinNode *)&m->mn_Node)->mln_Succ = &m->mn_Node;
        ((volatile struct MinNode *)&m->mn_Node)->mln_Pred = &m->mn_Node;
    }
}

int main(void)
{
    int64_t psig = AllocSignal(-1), csig = AllocSignal(-1), port, err;
    struct Message *m[3];
    struct MsgPort mp;

    if ((port = CreatePort("stash", psig)) < 0)
        return 1;
    while ((err = AcceptPort(&mp, (Handle)port, csig)) == ERR_NOTFOUND)
        Wait(1ULL << psig);
    if (err < 0)
        return 2;
    for (unsigned i = 0; i < 3; i++)
        if (!(m[i] = WaitPort(&mp)))
            return 3;
    for (unsigned i = 0; i < 3; i++) {
        ReplyMsg(&mp, m[i]);
        tamper(m[0], 200000);
    }
    while (!PortGone(&mp))
        tamper(m[0], 1);
    return 0;
}
