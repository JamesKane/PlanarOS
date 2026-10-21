/*
 * conformance/conn.c -- ports and connections with the raw kernel calls
 *
 * Run twice: arg 0 is the server; otherwise the client, with arg the
 * handle of a task in a third process it has no connection with. The server makes the
 * port "conn", accepts one connection and answers the client's signals;
 * the client checks the arena, writes into it for the server to read,
 * and trades signals. Then the server closes and the client must hear
 * of it. The server also leaves a port "conn2" behind when it exits,
 * with a connection it never accepted, which the client must hear has
 * gone; and the client leaves that connection open when it exits: the
 * kernel must close both. The client lends the server an Area, which
 * the server lets go of with ReleaseArea (v5-plan step 5). Exits 0 if every check passed, otherwise with the number of the
 * first that failed. Rings come with lib/exec (V2 step 2).
 */
#include <planar/ipc.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr unsigned ROUNDS = 200;
constexpr uint64_t PATTERN = 0x5a5a1234c0ffee00ULL;

static unsigned step;
static const char *role;

static int failed(const char *what)
{
    struct logbuf b = { 0 };

    log_str(&b, role);
    log_str(&b, ": check ");
    log_num(&b, step);
    log_str(&b, " failed: ");
    log_str(&b, what);
    KLog(b.text);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static uint64_t *heap_word(struct ConnArena *a)
{
    return (uint64_t *)((char *)a + a->ca_Heap);
}

static int server(void)
{
    int64_t psig = AllocSignal(-1), csig = AllocSignal(-1);
    struct ConnArena *a = nullptr;
    struct AreaInfo info;
    Handle port, conn;
    int64_t r;

    role = "conn server";
    CHECK(psig > 0 && csig > 0);
    r = CreatePort("conn", psig);
    CHECK(r > 0);
    port = (Handle)r;
    CHECK(CreatePort("conn", psig) == ERR_EXISTS);
    CHECK(CreatePort("other", 3) == ERR_BADARG);       /* a system signal */
    /* Left for the kernel to drop. Nobody can have connected to it yet:
     * the client tries only after its first round trip. */
    r = CreatePort("conn2", psig);
    CHECK(r > 0);
    CHECK(AcceptConn((Handle)r, csig, &a) == ERR_NOTFOUND);

    /* A client connecting signals the port. */
    while ((r = AcceptConn(port, csig, &a)) == ERR_NOTFOUND)
        Wait(1ULL << psig);
    CHECK(r > 0 && a);
    conn = (Handle)r;
    CHECK(a->ca_Magic == CONN_MAGIC && a->ca_Version == CONN_VERSION);
    CHECK(a->ca_Slots == 16 && a->ca_HeapSize >= 8192);
    CHECK(DeletePort(port) == 0);
    CHECK(DeletePort(port) == ERR_BADHANDLE);
    heap_word(a)[1] = FindTask(nullptr);       /* for the client's rights checks */

    /* The client's writes, through the same arena. */
    for (unsigned i = 0; i < ROUNDS; i++) {
        while (*(volatile uint64_t *)heap_word(a) != PATTERN + i)
            Wait(1ULL << csig);
        CHECK(SignalConn(conn) == 0);
    }

    /* What the client lent it, let go (ReleaseArea): only that, once. */
    {
        void *lent = (void *)heap_word(a)[2], *mine;
        int64_t area = AllocArea(4096, 0, &mine);

        CHECK(lent && QueryArea(lent, &info) == 0 && info.ai_Area == 0 && info.ai_Sharers == 2);
        CHECK(ReleaseArea(a) == ERR_RIGHTS);            /* the kernel's: the arena */
        CHECK(area > 0 && ReleaseArea(mine) == ERR_RIGHTS);
        CHECK(FreeArea((Handle)area) == 0);
        CHECK(ReleaseArea(lent) == 0);
        CHECK(QueryArea(lent, &info) == ERR_NOTFOUND);
        CHECK(ReleaseArea(lent) == ERR_NOTFOUND);
    }
    CHECK(CloseConn(conn) == 0);
    CHECK(SignalConn(conn) == ERR_BADHANDLE);
    return 0;
}

static int client(Handle stranger)
{
    int64_t sig = AllocSignal(-1);
    struct ConnArena *a = nullptr, *a2 = nullptr;
    struct AreaInfo info;
    Handle conn, server = 0;
    int64_t r, lent = 0;
    void *lent_at = nullptr;

    role = "conn client";
    CHECK(sig > 0);
    CHECK(OpenPort("conn", sig, 3, 0, &a) == ERR_BADARG);    /* not a power of two */
    CHECK(OpenPort("nobody", sig, 0, 0, &a) == ERR_NOTFOUND);
    /* The server may not have made its port yet. */
    while ((r = OpenPort("conn", sig, 16, 8192, &a)) == ERR_NOTFOUND)
        ;
    CHECK(r > 0 && a);
    conn = (Handle)r;
    CHECK(a->ca_Magic == CONN_MAGIC && a->ca_Slots == 16);
    CHECK(a->ca_Request == sizeof *a && a->ca_Reply > a->ca_Request);
    CHECK(a->ca_Heap + a->ca_HeapSize == a->ca_Size && a->ca_Size % 4096 == 0);
    CHECK(QueryArea(a, &info) == 0 && info.ai_Size == a->ca_Size && info.ai_Area == 0);

    for (unsigned i = 0; i < ROUNDS; i++) {
        *(volatile uint64_t *)heap_word(a) = PATTERN + i;
        CHECK(SignalConn(conn) == 0);
        Wait(1ULL << sig);
        /* Accepted, so shared by both (until the server's last answer). */
        if (i == 0) {
            void *mine;
            int64_t area = AllocArea(4096, 0, &mine);

            /* Another process's task: no Signal, but ShareArea while
             * there's a connection (docs/10-code-review.md M10). */
            server = (Handle)heap_word(a)[1];
            CHECK(area > 0 && server);
            CHECK(Signal(server, 1ULL << 20) == ERR_BADHANDLE);
            CHECK(ShareArea((Handle)area, server, AREAR_READ) == 0);
            CHECK(FreeArea((Handle)area) == 0);

            /* W^X holds across the two processes too (review M4). */
            area = AllocArea(4096, AREAF_EXEC, &mine);
            CHECK(area > 0);
            CHECK(ShareArea((Handle)area, server, AREAR_READ | AREAR_EXEC) == ERR_RIGHTS);
            CHECK(ShareArea((Handle)area, server, AREAR_READ | AREAR_WRITE) == 0);
            CHECK(ProtectArea((Handle)area, AREAR_READ | AREAR_EXEC) == ERR_RIGHTS);
            CHECK(FreeArea((Handle)area) == 0);
            CHECK(QueryArea(a, &info) == 0 && info.ai_Sharers == 2);

            /* One kept, lent to the server, which lets it go. */
            lent = AllocArea(4096, 0, &lent_at);
            CHECK(lent > 0 && ShareArea((Handle)lent, server, AREAR_READ | AREAR_WRITE) == 0);
            heap_word(a)[2] = (uint64_t)lent_at;
            CHECK(OpenPort("conn2", sig, 0, 0, &a2) > 0);
            CHECK(a2->ca_Closed == 0);
        }
    }

    /* The server closes: the arena says so, and the signal comes. */
    while (!(a->ca_Closed & CONNF_SERVER_GONE))
        Wait(1ULL << sig);
    CHECK(a->ca_Closed == CONNF_SERVER_GONE);
    CHECK(SignalConn(conn) == ERR_PORT_DIED);
    CHECK(SignalWaitConn(conn, 1ULL << sig) == 0);
    CHECK(QueryArea(a, &info) == 0 && info.ai_Sharers == 1);
    CHECK(QueryArea(lent_at, &info) == 0 && info.ai_Sharers == 1);    /* released */
    CHECK(FreeArea((Handle)lent) == 0);
    CHECK(CloseConn(conn) == 0);
    CHECK(QueryArea(a, &info) == ERR_NOTFOUND);
    {
        /* No connection with that process: no sharing with it either. */
        void *mine;
        int64_t area = AllocArea(4096, 0, &mine);

        CHECK(area > 0);
        CHECK(ShareArea((Handle)area, stranger, AREAR_READ) == ERR_RIGHTS);
        CHECK(Signal(stranger, 1ULL << 20) == ERR_BADHANDLE);
        CHECK(FreeArea((Handle)area) == 0);
    }

    /* The server's exit dropped "conn2", unaccepted. Exit with it open. */
    while (!(a2->ca_Closed & CONNF_SERVER_GONE))
        Wait(1ULL << sig);
    CHECK(OpenPort("conn2", sig, 0, 0, &a) == ERR_NOTFOUND);
    return 0;
}

int main(uint64_t arg)
{
    return arg ? client((Handle)arg) : server();
}
