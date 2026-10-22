/*
 * conformance/doorbell.c -- a page of a lease for a peer, and a lease's
 * DMA limit (v5-plan step 1)
 *
 * Plays a GPU server and its app, started by conformance/leaser.c. Bits
 * 0-1 of the argument say which.
 *
 * 1, the server: granted the virtio entropy device's lease (bits 32-63;
 * with the firmware call PSCI_VERSION, which it makes if bit 2 says
 * there's a conduit;
 * bits 8-11 the device's slot in the page, in 512-byte units), whose DMA
 * limit leaves room below it for one page. It allocates that page, and
 * keeps it to the end (quarantined at its death), and no second. Then it accepts the app's connection and shares the lease's
 * page with it, checking the refusals; the app reads the device through
 * it, the server takes it back and shares it again, and exits with the
 * share still in place, which its death must take away.
 *
 * 2, the app: bits 32-63 are a task in a process it has no connection
 * with (the leaser's). It reads the device's magic through the shared
 * page while it has it, and once the server has gone reads it again,
 * which must fault: the leaser checks that it did.
 *
 * The two step through a sequence number in their arena, by WaitAddr.
 * Exits 0 if every check passed, otherwise with the number of the first
 * that failed.
 */
#include <stdatomic.h>

#include <planar/ipc.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr uint64_t PAGE = 4096;
constexpr uint64_t MS = 1000000;
constexpr uint32_t VIRTIO_MAGIC = 0x74726976;

static unsigned step;
static const char *role = "doorbell";

static int failed(const char *what)
{
    KPrintF("%s: check %lu failed: %s", role, (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static void sleep_ms(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, 30);
    Wait(1ULL << 30);
}

/* The arena's words: the app's task, the step, where to read, what it read,
 * the stranger's task. */
enum { W_TASK, W_STEP, W_ADDR, W_READ, W_STRANGER };

static volatile uint64_t *words(struct ConnArena *a)
{
    return (volatile uint64_t *)((char *)a + a->ca_Heap);
}

static bool await(volatile uint64_t *w, uint64_t want)
{
    uint64_t v;

    while ((v = __atomic_load_n(&w[W_STEP], __ATOMIC_ACQUIRE)) != want)
        if (WaitAddr(&w[W_STEP], v, CurrentTime() + 2000 * MS) == ERR_TIMEOUT)
            return false;
    return true;
}

static void move(volatile uint64_t *w, uint64_t to)
{
    __atomic_store_n(&w[W_STEP], to, __ATOMIC_RELEASE);
    WakeAddr(&w[W_STEP], 1);
}

static int server(Handle l, uint64_t off, bool calls)
{
    int64_t psig = AllocSignal(-1), csig = AllocSignal(-1), r;
    struct ConnArena *a = nullptr;
    struct LeaseInfo li;
    volatile uint64_t *w;
    Handle port, app, stranger;
    void *at, *at2;

    role = "doorbell server";
    CHECK(psig > 0 && csig > 0);
    CHECK(QueryLease(l, &li) == 0 && li.li_State == LEASES_GRANTED && li.li_DmaLimit);

    /* Its firmware call (v5-plan step 2): the leaser allowed PSCI_VERSION,
     * which is 1.0 or later wherever there's PSCI. */
    {
        uint64_t x[4] = { 0, 0, 0, 0 };

        CHECK(li.li_Call == 0x84000000);
        CHECK(SetLeaseCall(l, 0) == ERR_BADHANDLE);        /* the grantee isn't the owner */
        CHECK(CallLease(l, (uint64_t *)8) == ERR_BADADDR);
        if (calls)
            CHECK(CallLease(l, x) == 0 && x[0] >= 0x10000 && x[0] < 0x20000);
        else
            CHECK(CallLease(l, x) == ERR_NOSYS);
    }

    /* Room below the limit for one page, and no more. */
    r = AllocDmaArea(l, PAGE, 0, &at);
    CHECK(r > 0 && (uint64_t)at + PAGE <= li.li_DmaLimit);
    CHECK(AllocDmaArea(l, PAGE, 0, &at2) == ERR_NOMEM);

    r = CreatePort("doorbell", psig);
    CHECK(r > 0);
    port = (Handle)r;
    while ((r = AcceptConn(port, csig, &a)) == ERR_NOTFOUND)
        Wait(1ULL << psig);
    CHECK(r > 0);
    w = words(a);
    CHECK(await(w, 1));
    app = (Handle)w[W_TASK];
    stranger = (Handle)w[W_STRANGER];

    /* Refusals. */
    CHECK(ShareLease(0, li.li_Base, PAGE, app) == ERR_BADHANDLE);
    CHECK(ShareLease(l, li.li_Base, PAGE, 0) == ERR_NOTFOUND);
    CHECK(ShareLease(l, li.li_Base, PAGE, FindTask(nullptr)) == ERR_BADARG);   /* itself */
    CHECK(ShareLease(l, li.li_Base, PAGE, stranger) == ERR_RIGHTS);     /* not connected */
    CHECK(ShareLease(l, li.li_Base - PAGE, PAGE, app) == ERR_BADARG);    /* outside it */
    CHECK(ShareLease(l, li.li_Base, li.li_Size + PAGE, app) == ERR_BADARG);
    CHECK(ShareLease(l, li.li_Base + 8, PAGE, app) == ERR_BADARG);
    CHECK(ShareLease(l, li.li_Base, 0, app) == ERR_BADARG);
    CHECK(UnshareLease(l, li.li_Base, app) == ERR_NOTFOUND);

    /* Shared: the app reads the device through it. */
    CHECK(ShareLease(l, li.li_Base, PAGE, app) == 0);
    CHECK(ShareLease(l, li.li_Base, PAGE, app) == ERR_EXISTS);
    CHECK(QueryLease(l, &li) == 0 && li.li_Shares == 1);
    w[W_ADDR] = li.li_Base + off;
    move(w, 2);
    CHECK(await(w, 3));
    CHECK(w[W_READ] == VIRTIO_MAGIC);

    /* Taken back, and shared again. */
    CHECK(UnshareLease(l, li.li_Base, app) == 0);
    CHECK(UnshareLease(l, li.li_Base, app) == ERR_NOTFOUND);
    CHECK(QueryLease(l, &li) == 0 && li.li_Shares == 0);
    CHECK(ShareLease(l, li.li_Base, PAGE, app) == 0);
    w[W_READ] = 0;
    move(w, 4);
    CHECK(await(w, 5));
    CHECK(w[W_READ] == VIRTIO_MAGIC);

    /* Gone with the share in place: the grant's end takes it. */
    return 0;
}

static int app(Handle stranger)
{
    int64_t sig = AllocSignal(-1), r;
    struct ConnArena *a = nullptr;
    volatile uint64_t *w;

    role = "doorbell app";
    CHECK(sig > 0);
    while ((r = OpenPort("doorbell", sig, 0, 0, &a)) == ERR_NOTFOUND)
        sleep_ms(1);
    CHECK(r > 0);
    w = words(a);
    w[W_TASK] = FindTask(nullptr);
    w[W_STRANGER] = stranger;
    move(w, 1);
    CHECK(await(w, 2));
    w[W_READ] = *(volatile uint32_t *)w[W_ADDR];
    move(w, 3);
    CHECK(await(w, 4));
    w[W_READ] = *(volatile uint32_t *)w[W_ADDR];
    move(w, 5);

    /* The server ends; its share must go with it. */
    for (unsigned i = 0; i < 2000 && !(atomic_load(&a->ca_Closed) & CONNF_SERVER_GONE); i++)
        sleep_ms(1);
    CHECK(atomic_load(&a->ca_Closed) & CONNF_SERVER_GONE);
    sleep_ms(50);                   /* its reclamation, which follows the close */
    (void)*(volatile uint32_t *)w[W_ADDR];
    return failed("the page was still there after the server died");
}

int main(uint64_t arg)
{
    switch (arg & 3) {
    case 1:
        return server((Handle)(arg >> 32), (arg >> 8 & 0xf) * 512, arg >> 2 & 1);
    case 2:
        return app((Handle)(arg >> 32));
    default:
        return 99;
    }
}
