/*
 * servers/hwdb/killtest.c -- hwdb's test mode: the kill test (§11.7, §11.10)
 *
 * The kernel starts hwdb with HWDB_TEST. Once every driver that can bind
 * has, this:
 *
 * 1. Starts a client task per class port the drivers published. Each
 *    reads its device over and over, and when its connection dies with
 *    ERR_PORT_DIED, reconnects, as a class library would (§11.9).
 * 2. Kills the units that serve them, in turn, ROUNDS times each, at
 *    random points: some while still binding, most while serving. Each
 *    death goes through server.c's teardown, which checks the kernel
 *    masked and unbound the lines and quarantined the DMA, and that
 *    revoking the leases freed it; a restarted driver binding its line
 *    again shows the old binding went. Between rounds hwdb's own handles
 *    must not grow.
 * 3. Has one unit fault in Bind on every start, and checks it's given up
 *    on after FAILS failures, the restarts spaced by the backoff; then
 *    starts it afresh.
 * 4. Stops the clients, which must each have recovered from every death
 *    they saw; then kills every unit for good and closes the leases.
 *
 * Then hwdb exits: 0 if every check passed. The kernel checks nothing it
 * made outlived it (kernel/selftest.c, V4.3).
 */
#include <stddef.h>

#include <planar/syscalls.h>

#include "server.h"

void *memset(void *dst, int c, size_t n);

constexpr unsigned CLIENTS = 8;
constexpr unsigned ROUNDS = 12;         /* kills per unit with clients */
constexpr uint64_t Y2020 = 1577836800;
constexpr int64_t SIG_CLIENT = 20;      /* a client's connection */
constexpr int64_t SIG_SLEEP = 21;       /* a client's alarm */

enum phase : uint8_t { T_SETTLE, T_ROUNDS, T_GIVEUP, T_RETRY, T_DRAIN, T_STOP, T_END };

struct client {
    char port[16];
    bool rtc;
    struct unit *unit;
    _Atomic bool stop, done;
    unsigned answers, deaths, recovered, bad, busy;
};

static struct client clients[CLIENTS];
static unsigned nclients;
static enum phase phase;
static struct unit *live[64], *victim;
static unsigned nlive, round, rounds, binding, victim_starts;
static uint64_t wake, phase_at, seed;
static uint32_t handles;            /* hwdb's, before the clients */

static uint64_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

static uint32_t own_handles(void)
{
    struct ProcessInfo pi;

    return QueryProcess(0, &pi) == 0 ? pi.pi_Handles : UINT32_MAX;
}

static void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_SLEEP);
    Wait(1ULL << SIG_SLEEP);
}

static bool plausible(const struct client *c, const struct DrvRequest *r)
{
    uint64_t v = 0;
    unsigned nonzero = 0;

    if (c->rtc) {
        if (r->dr_Len != 8)
            return false;
        for (unsigned i = 0; i < 8; i++)
            v |= (uint64_t)r->dr_Data[i] << (8 * i);
        return v > Y2020;
    }
    for (unsigned i = 0; i < r->dr_Len; i++)
        nonzero += r->dr_Data[i] != 0;
    return r->dr_Len == 32 && nonzero > 16;
}

/* A class library's loop, at its simplest: read; on a death, reconnect. */
static void client(uint64_t arg)
{
    struct client *c = &clients[arg];
    struct MsgPort mp;
    struct DrvRequest *m = nullptr;
    bool open = false, lost = false;
    uint64_t since = CurrentTime();

    while (!c->stop) {
        int64_t e;

        if (!open) {
            e = ConnectPort(&mp, c->port, SIG_CLIENT, 4, 4096);
            if (e == ERR_NOTFOUND) {
                if (CurrentTime() - since > 3000 * MS) {
                    c->bad++;           /* gone for longer than any restart takes */
                    since = CurrentTime();
                }
                nap(1);
                continue;
            }
            if (e != 0 || !(m = AllocMsg(&mp, sizeof *m))) {
                c->bad++;
                if (e == 0)
                    ClosePort(&mp);
                nap(1);
                continue;
            }
            open = true;
            if (lost)
                c->recovered++;
            lost = false;
        }
        memset(m, 0, sizeof *m);
        m->dr_Msg.mn_Length = sizeof *m;
        m->dr_Op = DRV_READ;
        m->dr_Len = c->rtc ? 8 : 32;
        e = DoMsg(&mp, &m->dr_Msg);
        if (e == ERR_PORT_DIED || m->dr_Msg.mn_Error == ERR_PORT_DIED) {
            c->deaths++;
            ClosePort(&mp);
            open = false;
            lost = true;
            since = CurrentTime();
            continue;
        }
        if (e == 0 && m->dr_Msg.mn_Error == 0 && plausible(c, m))
            c->answers++;
        else if (m->dr_Msg.mn_Error == ERR_BUSY)
            c->busy++;
        else
            c->bad++;
        nap(1);
    }
    if (open)
        ClosePort(&mp);
    c->done = true;
    ExitTask(0);
}

static void say_num(const char *a, uint64_t n, const char *b)
{
    struct hw_line l = { 0 };

    hw_put(&l, a);
    hw_put_dec(&l, n);
    hw_put(&l, b);
    say(l.text);
}

static void fail(const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: kill test: check failed: ");
    hw_put(&l, what);
    say(l.text);
    hwdb.errors++;
}

static void settle(void)
{
    uint32_t n = hwdb.g.nleases;

    seed = hwdb.now | 1;
    hwdb_report();
    for (uint32_t i = 0; i < n; i++) {
        struct unit *u = &hwdb.units[i];
        const struct hw_lease *ls = &hwdb.g.leases[i];
        bool any = false;

        if (u->state != U_RUNNING)
            continue;
        for (unsigned k = 0; k < ls->nnodes && nclients < CLIENTS; k++) {
            struct client *c = &clients[nclients];

            if (!(u->accepted & 1u << k) || !u->ports[k][0])
                continue;
            for (unsigned q = 0; q < sizeof c->port; q++)
                c->port[q] = u->ports[k][q];
            c->rtc = hw_strneq(u->ports[k], "rtc.", 4);
            c->unit = u;
            nclients++;
            any = true;
        }
        if (any && nlive < sizeof live / sizeof live[0])
            live[nlive++] = u;
    }
    if (!nlive) {
        say("hwdb: kill test: no driver here serves a device; nothing to kill");
        phase = T_STOP;
        return;
    }
    handles = own_handles();
    for (unsigned i = 0; i < nclients; i++)
        if (CreateTask(client, i) < 0)      /* its handle goes when it ends */
            fail("a client task couldn't be made");
    rounds = ROUNDS * nlive;
    wake = hwdb.now + 20 * MS;      /* let the clients get going */
    phase = T_ROUNDS;
}

static void kill_rounds(void)
{
    struct unit *u;

    /* A restart to kill while it binds: soon after it's made, or as soon
     * as it's seen, if it bound first. */
    if (victim) {
        if (victim->starts == victim_starts)
            return;                 /* not restarted yet */
        if (victim->state == U_RUNNING ||
            (victim->state == U_STARTING && hwdb.now >= victim->started + wake)) {
            binding += victim->state == U_STARTING;
            unit_kill(victim);
            victim = nullptr;
            wake = hwdb.now + (2 + rnd() % 8) * MS;
        }
        return;
    }
    if (hwdb_busy() || hwdb.now < wake) {
        return;
    }
    if (round > 0 && own_handles() > handles + 2 * nclients)
        fail("hwdb's handles grew across a restart");
    if (round == rounds) {
        phase = T_GIVEUP;
        return;
    }
    u = live[round % nlive];
    if (u->state != U_RUNNING) {
        fail("a unit wasn't running between rounds");
        phase = T_GIVEUP;
        return;
    }
    round++;
    unit_kill(u);
    if (rnd() % 3 == 0 && round < rounds) {
        victim = u;                 /* and its restart, a round of its own */
        victim_starts = u->starts;
        wake = rnd() % 2000000;     /* ns after it starts */
        round++;
    } else {
        wake = hwdb.now + (2 + rnd() % 8) * MS;
    }
}

static void give_up(void)
{
    if (phase == T_GIVEUP && !hwdb_busy()) {
        victim = live[0];
        for (unsigned i = 0; i < nlive; i++)
            if (hw_strneq(unit_driver(live[i]), "rtc-", 4))
                victim = live[i];
        victim->flags = DRVF_CRASH;
        victim->nfails = 0;
        unit_kill(victim);
        phase_at = hwdb.now;
        phase = T_RETRY;
        return;
    }
    if (phase != T_RETRY)
        return;
    if (victim->state == U_DONE) {
        uint64_t gap = 0;
        bool spaced = victim->nfails == FAILS;

        for (unsigned i = 1; i < victim->nfails; i++) {
            gap = victim->fail_at[i] - victim->fail_at[i - 1];
            spaced &= gap >= (BACKOFF_MS << (i - 1)) * MS;
        }
        if (!spaced)
            fail("the unit wasn't given up on after 5 failures, spaced by the backoff");
        {
            const struct hw_lease *ls = &hwdb.g.leases[victim - hwdb.units];
            unsigned failed = 0, bound = 0;

            for (unsigned i = 0; i < ls->nnodes; i++) {
                failed += hwdb.g.nodes[ls->nodes[i]].state == HW_S_FAILED;
                bound += hwdb.g.nodes[ls->nodes[i]].state == HW_S_BOUND;
            }
            if (!failed || bound)
                fail("a unit given up on left its devices bound");
        }
        say_num("hwdb: kill test: a driver faulting in Bind was given up on after 5 failures "
                "in ", (hwdb.now - phase_at) / MS, " ms; starting it afresh");
        victim->flags = 0;
        unit_retry(victim);
        phase = T_DRAIN;
        phase_at = hwdb.now;
    } else if (hwdb.now - phase_at > 3000 * MS) {
        fail("the faulting unit was never given up on");
        phase = T_DRAIN;
    }
}

static void drain(void)
{
    bool all = true;
    unsigned answers = 0, deaths = 0;

    if (victim && victim->state != U_RUNNING) {
        if (hwdb.now - phase_at > 3000 * MS) {
            fail("the unit given up on didn't start again");
            victim = nullptr;
        }
        return;
    }
    if (hwdb.now < phase_at + 50 * MS)
        return;                     /* time for its client to come back */
    for (unsigned i = 0; i < nclients; i++)
        clients[i].stop = true;
    for (unsigned i = 0; i < nclients; i++)
        all &= clients[i].done;
    if (!all)
        return;
    for (unsigned i = 0; i < nclients; i++) {
        const struct client *c = &clients[i];

        answers += c->answers;
        deaths += c->deaths;
        if (c->answers == 0 || c->deaths == 0)
            fail("a client saw no answers, or no deaths");
        if (c->recovered != c->deaths)
            fail("a client didn't recover from every death");
        if (c->bad)
            fail("a client had a wrong answer, or no driver for 3 s");
    }
    if (own_handles() != handles) {
        struct hw_line l = { 0 };

        hw_put(&l, "hwdb's handles aren't what they were before the test: ");
        hw_put_dec(&l, handles);
        hw_put(&l, ", now ");
        hw_put_dec(&l, own_handles());
        fail(l.text);
    }
    {
        struct hw_line l = { 0 };
        unsigned starts = 0;

        for (unsigned i = 0; i < nlive; i++)
            starts += live[i]->starts;
        hw_put(&l, "hwdb: kill test: ");
        hw_put_dec(&l, rounds);
        hw_put(&l, " kills (");
        hw_put_dec(&l, binding);
        hw_put(&l, " while binding) of ");
        hw_put_dec(&l, nlive);
        hw_put(&l, " drivers, ");
        hw_put_dec(&l, starts);
        hw_put(&l, " starts; ");
        hw_put_dec(&l, nclients);
        hw_put(&l, " clients read ");
        hw_put_dec(&l, answers);
        hw_put(&l, " times and reconnected after each of ");
        hw_put_dec(&l, deaths);
        hw_put(&l, " deaths; ");
        hw_put_dec(&l, hwdb.quarantined);
        hw_put(&l, " DMA pages held in quarantine until the device was stopped");
        say(l.text);
    }
    phase = T_STOP;
}

static void stop_all(void)
{
    bool any = false;

    if (phase == T_STOP) {
        hwdb.stopping = true;
        for (uint32_t i = 0; i < hwdb.g.nleases; i++)
            unit_kill(&hwdb.units[i]);
        phase = T_END;
    }
    for (uint32_t i = 0; i < hwdb.g.nleases; i++)
        any |= hwdb.units[i].proc != 0;
    if (!any)
        hwdb.finished = true;
}

static void step(void);

void test_step(void)
{
    enum phase was;

    do {
        was = phase;
        step();
    } while (phase != was && !hwdb.finished);  /* a new phase may act at once */
}

static void step(void)
{
    switch (phase) {
    case T_SETTLE:
        if (!hwdb_busy())
            settle();
        break;
    case T_ROUNDS:
        kill_rounds();
        break;
    case T_GIVEUP:
    case T_RETRY:
        give_up();
        break;
    case T_DRAIN:
        drain();
        break;
    case T_STOP:
    case T_END:
        stop_all();
        break;
    }
}

/* Ticks while waiting on the clients' and the restarts' own pace. */
uint64_t test_deadline(void)
{
    switch (phase) {
    case T_ROUNDS:
        if (victim)
            return hwdb.now + MS / 4;
        return wake > hwdb.now ? wake : hwdb.now + 1 * MS;
    case T_RETRY:
    case T_DRAIN:
        return hwdb.now + 5 * MS;
    default:
        return 0;
    }
}
