/*
 * conformance/crowd.c -- a server's accepts, past a broken client and when full (§10.8)
 *
 * The server (this program, argument 1) makes its port and naps, so that
 * four clients queue before it accepts any: one whose arena header it
 * broke first (a client may write it until it's accepted), two good
 * ones, and one more than the server's two places. AcceptPort must close
 * the broken one and go on to the rest, rather than stop there and leave
 * them unanswered with the port signalled once; the server closes the
 * extra one, which hears ERR_PORT_DIED rather than nothing. Exits 0, or 1
 * with the line.
 */
#include <planar/msg.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr int64_t SIG_CONN = 16;
constexpr int64_t SIG_PORT = 17;
constexpr int64_t SIG_NAP = 22;
constexpr int64_t SIG_DEAD = 23;
constexpr uint64_t MS = 1000000;
constexpr unsigned PLACES = 2;

#define CHECK(c) \
    do { \
        if (!(c)) { \
            KPrintF("crowd: check at line %d failed", __LINE__); \
            return 1; \
        } \
    } while (0)

static void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_NAP);
    Wait(1ULL << SIG_NAP);
}

static int server(void)
{
    struct MsgPort clients[PLACES + 1];
    unsigned n = 0;
    int64_t port = CreatePort("crowd", SIG_PORT);

    if (port < 0)
        return 1;
    nap(200);
    for (;;) {
        /* As a server does: the wait takes the port's signal, for all
         * four at once, then the accepts. */
        WaitPorts(clients, n, 1ULL << SIG_PORT | 1ULL << SIG_CONN);
        while (AcceptPort(&clients[n < PLACES ? n : PLACES], (Handle)port, SIG_CONN) == 0) {
            if (n < PLACES)
                n++;
            else
                ClosePort(&clients[PLACES]);
        }
        for (unsigned i = 0; i < n; i++) {
            struct Message *m;

            while ((m = GetMsg(&clients[i])))
                ReplyMsg(&clients[i], m);
        }
    }
}

int main(uint64_t arg)
{
    struct MsgPort good[2], extra;
    struct ConnArena *a;
    struct Message *m[3];
    int64_t srv, bad;

    if (arg == 1)
        return server();
    CHECK((srv = CreateProcess("crowd", 1, SIG_DEAD, 0)) > 0);
    CHECK((bad = OpenPort("crowd", SIG_CONN, 4, 4096, &a, CurrentTime() + 1000 * MS)) > 0);
    a->ca_Magic = 0;
    CHECK(ConnectPortUntil(&good[0], "crowd", SIG_CONN, 4, 4096, CurrentTime() + 1000 * MS) == 0);
    CHECK(ConnectPortUntil(&good[1], "crowd", SIG_CONN, 4, 4096, CurrentTime() + 1000 * MS) == 0);
    CHECK(ConnectPortUntil(&extra, "crowd", SIG_CONN, 4, 4096, CurrentTime() + 1000 * MS) == 0);
    for (unsigned i = 0; i < 3; i++) {
        struct MsgPort *mp = i < 2 ? &good[i] : &extra;

        CHECK((m[i] = AllocMsg(mp, sizeof *m[i])));
        m[i]->mn_Length = sizeof *m[i];
    }
    CHECK(DoMsgUntil(&good[0], m[0], CurrentTime() + 2000 * MS) == 0);
    CHECK(DoMsgUntil(&good[1], m[1], CurrentTime() + 2000 * MS) == 0);
    CHECK(DoMsgUntil(&extra, m[2], CurrentTime() + 2000 * MS) == ERR_PORT_DIED);
    KPrintF("crowd: a broken client passed over, two served, one past the places told");
    ClosePort(&good[0]);
    ClosePort(&good[1]);
    ClosePort(&extra);
    CloseConn((Handle)bad);
    KillProcess((Handle)srv);
    CloseHandle((Handle)srv);
    return 0;
}
