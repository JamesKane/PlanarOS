/*
 * conformance/deadline.c -- waits with deadlines (V6 step 7, VQ28)
 *
 * OpenPort's wait for a name: none at deadline 0, ERR_TIMEOUT at one
 * past, and woken when the port is made, not by polling. WaitUntil:
 * the deadline, a signal already there, and a poll. Then DoMsgUntil
 * against a server (this program, argument 1) that answers each message
 * after the delay it asks for: an answer in time, one too late
 * (ERR_TIMEOUT, the message in mp_Lost), ERR_BUSY until the late answer
 * comes and is dropped, and ERR_PORT_DIED, not ERR_BUSY, when the
 * server dies holding one. Last, a waiter in OpenPort (argument 2) that
 * no port will ever wake is killed, and must die at once. Then what a
 * DoMsgUntil costs against a DoMsg. Exits 0, or 1 with the line.
 */
#include <planar/msg.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr int64_t SIG_CONN = 16;
constexpr int64_t SIG_PORT = 17;
constexpr int64_t SIG_W = 20;
constexpr int64_t SIG_DEAD = 21;
constexpr int64_t SIG_NAP = 22;
constexpr uint64_t MS = 1000000;

struct DlMsg {
    struct Message dl_Msg;
    uint32_t dl_Delay;              /* ms before the answer */
    uint32_t dl_Pad;
};

#define CHECK(c) \
    do { \
        if (!(c)) { \
            KPrintF("deadline: check at line %d failed", __LINE__); \
            return 1; \
        } \
    } while (0)

static void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_NAP);
    Wait(1ULL << SIG_NAP);
}

/* Argument 1: the port, made late; each message answered after its delay. */
static int server(void)
{
    struct MsgPort mp;
    struct DlMsg *m;
    int64_t port;

    nap(100);
    if ((port = CreatePort("deadline", SIG_PORT)) < 0)
        return 1;
    while (AcceptPort(&mp, (Handle)port, SIG_CONN) != 0)
        Wait(1ULL << SIG_PORT);
    while ((m = (struct DlMsg *)WaitPort(&mp))) {
        if (m->dl_Delay)
            nap(m->dl_Delay);
        ReplyMsg(&mp, &m->dl_Msg);
    }
    ClosePort(&mp);
    DeletePort((Handle)port);
    return 0;
}

/* Argument 2: waiting for a port no one makes, until killed. */
static int waiter(void)
{
    struct ConnArena *a;

    OpenPort("never", SIG_CONN, 0, 0, &a, DEADLINE_NONE);
    return 1;                       /* only a kill ends the wait */
}

static bool dead(Handle p, uint64_t within_ms, struct ProcessInfo *pi)
{
    for (uint64_t until = CurrentTime() + within_ms * MS;;) {
        if (QueryProcess(p, pi) == 0 && pi->pi_State >= PROCS_EXITED)
            return true;
        if (CurrentTime() >= until)
            return false;
        WaitUntil(1ULL << SIG_DEAD, CurrentTime() + MS);
    }
}

int main(uint64_t arg)
{
    struct ConnArena *a;
    struct MsgPort mp;
    struct DlMsg *m1, *m2;
    struct ProcessInfo pi;
    uint64_t t, el, plain, until;
    int64_t srv, w;

    if (arg == 1)
        return server();
    if (arg == 2)
        return waiter();

    /* OpenPort's wait, with no port. */
    CHECK(OpenPort("deadline", SIG_CONN, 0, 0, &a, 0) == ERR_NOTFOUND);
    t = CurrentTime();
    CHECK(OpenPort("deadline", SIG_CONN, 0, 0, &a, t + 50 * MS) == ERR_TIMEOUT);
    el = CurrentTime() - t;
    CHECK(el >= 50 * MS && el < 1000 * MS);
    CHECK(OpenPort("deadline", SIG_CONN, 0, 0, &a, t) == ERR_TIMEOUT);     /* past */

    /* WaitUntil. */
    SetSignal(0, 1ULL << SIG_W);
    t = CurrentTime();
    CHECK(WaitUntil(1ULL << SIG_W, t + 30 * MS) == 0);
    CHECK(CurrentTime() - t >= 30 * MS);
    Signal(FindTask(nullptr), 1ULL << SIG_W);
    t = CurrentTime();
    CHECK(WaitUntil(1ULL << SIG_W, t + 5000 * MS) == 1ULL << SIG_W);
    CHECK(CurrentTime() - t < 1000 * MS);
    CHECK(WaitUntil(1ULL << SIG_W, 0) == 0);                 /* a poll: nothing there */
    Signal(FindTask(nullptr), 1ULL << SIG_W);
    CHECK(WaitUntil(1ULL << SIG_W, 0) == 1ULL << SIG_W);     /* a poll: there */

    /* The server makes its port 100 ms in: the wait ends when it does. */
    SetSignal(0, 1ULL << SIG_DEAD);
    CHECK((srv = CreateProcess("deadline", 1, SIG_DEAD, 0)) > 0);
    t = CurrentTime();
    CHECK(ConnectPortUntil(&mp, "deadline", SIG_CONN, 4, 4096, t + 5000 * MS) == 0);
    el = CurrentTime() - t;
    CHECK(el >= 90 * MS && el < 1000 * MS);
    CHECK((m1 = AllocMsg(&mp, sizeof *m1)) && (m2 = AllocMsg(&mp, sizeof *m2)));
    m1->dl_Msg.mn_Length = m2->dl_Msg.mn_Length = sizeof *m1;

    /* In time; too late; busy until the late answer, which is dropped. */
    m1->dl_Delay = 0;
    CHECK(DoMsgUntil(&mp, &m1->dl_Msg, CurrentTime() + 1000 * MS) == 0);
    m1->dl_Delay = 300;
    t = CurrentTime();
    CHECK(DoMsgUntil(&mp, &m1->dl_Msg, t + 50 * MS) == ERR_TIMEOUT);
    el = CurrentTime() - t;
    CHECK(el >= 50 * MS && el < 250 * MS);
    CHECK(mp.mp_Lost == &m1->dl_Msg);
    m2->dl_Delay = 0;
    CHECK(DoMsgUntil(&mp, &m2->dl_Msg, CurrentTime() + 1000 * MS) == ERR_BUSY);
    CHECK(DoMsg(&mp, &m2->dl_Msg) == ERR_BUSY);
    nap(400);
    CHECK(DoMsgUntil(&mp, &m2->dl_Msg, CurrentTime() + 1000 * MS) == 0);
    CHECK(mp.mp_Lost == nullptr && GetMsg(&mp) == nullptr);
    m1->dl_Delay = 10;
    CHECK(DoMsgUntil(&mp, &m1->dl_Msg, DEADLINE_NONE) == 0);

    /* What a deadline costs a round trip, once both are warm. */
    m1->dl_Delay = 0;
    for (unsigned i = 0; i < 200; i++)
        CHECK(DoMsg(&mp, &m1->dl_Msg) == 0 &&
              DoMsgUntil(&mp, &m1->dl_Msg, CurrentTime() + 1000 * MS) == 0);
    t = CurrentTime();
    for (unsigned i = 0; i < 2000; i++)
        CHECK(DoMsg(&mp, &m1->dl_Msg) == 0);
    plain = (CurrentTime() - t) / 2000;
    t = CurrentTime();
    for (unsigned i = 0; i < 2000; i++)
        CHECK(DoMsgUntil(&mp, &m1->dl_Msg, CurrentTime() + 1000 * MS) == 0);
    el = (CurrentTime() - t) / 2000;

    /* Lost, and then the server dies: gone, not busy. */
    m1->dl_Delay = 5000;
    CHECK(DoMsgUntil(&mp, &m1->dl_Msg, CurrentTime() + 30 * MS) == ERR_TIMEOUT);
    CHECK(KillProcess((Handle)srv) == 0);
    CHECK(dead((Handle)srv, 1000, &pi));
    CloseHandle((Handle)srv);
    CHECK(DoMsgUntil(&mp, &m2->dl_Msg, CurrentTime() + 1000 * MS) == ERR_PORT_DIED);
    ClosePort(&mp);

    /* A task waiting in OpenPort for ever, killed. */
    CHECK((w = CreateProcess("deadline", 2, SIG_DEAD, 0)) > 0);
    nap(20);
    CHECK(QueryProcess((Handle)w, &pi) == 0 && pi.pi_State == PROCS_RUNNING);
    t = CurrentTime();
    CHECK(KillProcess((Handle)w) == 0);
    CHECK(dead((Handle)w, 1000, &pi) && pi.pi_State == PROCS_KILLED);
    until = CurrentTime() - t;
    CloseHandle((Handle)w);
    CHECK(OpenPort("never", SIG_CONN, 0, 0, &a, 0) == ERR_NOTFOUND);

    KPrintF("deadline: OpenPort waited for a name and woke when it came; WaitUntil; "
            "DoMsgUntil's timeout, mp_Lost, ERR_BUSY, the late answer dropped");
    KPrintF("deadline: a waiter in OpenPort killed in %lu us; a round trip by DoMsg %lu ns, "
            "by DoMsgUntil %lu ns", until / 1000, plain, el);
    return 0;
}
