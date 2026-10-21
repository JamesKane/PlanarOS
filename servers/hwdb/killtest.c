/*
 * servers/hwdb/killtest.c -- hwdb's test mode: the kill test (§11.7, §11.10)
 *
 * The kernel starts hwdb with HWDB_TEST. Once every driver that can bind
 * has, this:
 *
 * 0. Measures each block device with blkbench (conformance/blkbench.c),
 *    one at a time, while nothing else runs. Then starts dos, the file
 *    server, and runs dostest against it, then the Shell from the disk
 *    on a script there (C:shell -s SYS:EFI/planar/S/test), which must end
 *    0; dos stays through the kills, its devices restarting under it,
 *    and is stopped at the end.
 * 1. Starts a client task per class port the drivers published. Each
 *    reads its device over and over, and when its connection dies with
 *    ERR_PORT_DIED, reconnects, as a class library would (§11.9). A
 *    block device's client is lib/blk itself, reading at random in the
 *    disk's second partition with no pause, so that kills come with
 *    I/O in flight; every block it gets must hold its own number
 *    (tools/mkdisk.py).
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

#include <planar/blk.h>
#include <planar/dos.h>
#include <planar/syscalls.h>

#include "server.h"

void *memset(void *dst, int c, size_t n);

constexpr unsigned CLIENTS = 8;
constexpr unsigned ROUNDS = 12;         /* kills per unit with clients */
constexpr uint64_t Y2020 = 1577836800;
constexpr int64_t SIG_CLIENT = 20;      /* a client's connection */
constexpr int64_t SIG_SLEEP = 21;       /* a client's alarm */

enum phase : uint8_t { T_SETTLE, T_BENCH, T_DOS, T_SHELL, T_ROUNDS, T_GIVEUP, T_RETRY, T_DRAIN, T_POWER,
                       T_DISPLAY, T_INPUT, T_GPU, T_STOP, T_END };

enum kind : uint8_t { K_RNG, K_RTC, K_BLOCK, K_CON };

struct client {
    char port[16];
    enum kind kind;
    struct unit *unit;
    _Atomic bool stop, done;
    unsigned answers, deaths, recovered, bad, busy;
    int64_t last_bad;               /* the last error it saw, for the report */
};

static struct client clients[CLIENTS];
static unsigned nclients;
static enum phase phase;
static struct unit *live[64], *victim;
static unsigned nlive, round, rounds, binding, victim_starts;
static uint64_t wake, phase_at, seed, all_done_at;
static Handle bench;                /* blkbench, measuring a block device; then dostest */
static Handle dos;                  /* the file server, from T_DOS to the end */
static unsigned measured;           /* block devices blkbench measured well */
static bool files_ok, shell_ok;     /* dostest, and the Shell's script, passed */
static unsigned benched;            /* clients whose devices it has measured */
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

    if (c->kind == K_CON)
        return r->dr_Len == 0;
    if (c->kind == K_RTC) {
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

/* lib/blk's reads, which reconnect and retry by themselves: a death
 * shows as a reconnect, and a recovery as a good read after one. */
static uint8_t blocks[CLIENTS][8 * 4096];

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << 8 * i;
    return v;
}

static void block_client(struct client *c, uint8_t *buf)
{
    struct blk b;
    bool check = false;
    uint32_t seen = 0, lost = 0;
    uint64_t r = (uint64_t)buf | 1;     /* its own: the clients run at once */

    /* Opening may meet a kill too: tried again for as long as a restart takes. */
    for (uint64_t since = CurrentTime();;) {
        int64_t e = blk_open(&b, c->port, SIG_CLIENT, 256 * 1024);

        if (e == 0 && b.block_size <= 4096 && b.dev_blocks > 16) {
            /* The test disk's second partition (tools/mkdisk.py), whose
             * blocks hold their own numbers; any other disk is only read,
             * anywhere, and nothing it holds can be checked. */
            int64_t p = blk_partition(&b, 2);

            if (p == ERR_PORT_DIED)
                e = p;
            else if (p == 0 && (e = blk_read(&b, 0, 1, buf)) == 0)
                check = le64(buf) == b.first;
            if (e == 0 && !check) {
                b.first = 0;
                b.blocks = b.dev_blocks;
            }
            if (e == 0)
                break;
        }
        if (e == 0)
            blk_close(&b);
        if (c->stop || CurrentTime() - since > BLK_GONE_MS * MS) {
            c->bad++;
            c->last_bad = e;
            c->done = true;
            ExitTask(0);
        }
        nap(1);
    }
    while (!c->stop) {
        uint32_t n;
        uint64_t at;
        bool good;

        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        n = 1 + (uint32_t)(r % 8);
        at = (r >> 8) % (b.blocks - n);

        int64_t e = blk_read(&b, at, n, buf);

        if (e != 0) {
            c->bad++;
            c->last_bad = e;
            nap(1);
            continue;
        }
        good = true;
        for (uint32_t k = 0; k < n && check; k++)
            good &= le64(buf + k * b.block_size) == b.first + at + k;
        if (b.reconnects != seen) {
            c->deaths += b.reconnects - seen;
            lost += b.reconnects - seen;
            seen = b.reconnects;
        }
        if (good) {
            c->answers++;
            c->recovered += lost;
            lost = 0;
        } else {
            c->bad++;
            c->last_bad = 1;        /* a block that wasn't its own */
        }
    }
    blk_close(&b);
    c->done = true;
    ExitTask(0);
}

/* A class library's loop, at its simplest: read; on a death, reconnect. */
static void client(uint64_t arg)
{
    struct client *c = &clients[arg];

    if (c->kind == K_BLOCK)
        block_client(c, blocks[arg]);
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
                    c->last_bad = ERR_NOTFOUND;
                    since = CurrentTime();
                }
                nap(1);
                continue;
            }
            if (e != 0 || !(m = AllocMsg(&mp, sizeof *m))) {
                c->bad++;
                c->last_bad = e ? e : ERR_NOMEM;
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
        m->dr_Op = c->kind == K_CON ? DRV_WRITE : DRV_READ;   /* a console's: nothing, written */
        m->dr_Len = c->kind == K_RTC ? 8 : c->kind == K_CON ? 0 : 32;
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
            c->bad++, c->last_bad = e ? e : m->dr_Msg.mn_Error ? m->dr_Msg.mn_Error : 1;
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
    hwdb_facts();
    for (uint32_t i = 0; i < n; i++) {
        struct unit *u = &hwdb.units[i];
        const struct hw_lease *ls = &hwdb.g.leases[i];
        bool any = false;

        if (u->state != U_RUNNING)
            continue;
        for (unsigned k = 0; k < ls->nnodes && nclients < CLIENTS; k++) {
            struct client *c = &clients[nclients];

            if (!(u->accepted & 1u << k) || !u->ports[k][0] || hw_streq(u->ports[k], "power"))
                continue;           /* power's is the power test's (power.c) */
            if (hw_strneq(u->ports[k], "gpu.", 4)) {
                any = true;         /* killed in the rounds; its client is the GPU test's (gpu.c) */
                continue;
            }
            for (unsigned q = 0; q < sizeof c->port; q++)
                c->port[q] = u->ports[k][q];
            c->kind = hw_strneq(u->ports[k], "rtc.", 4)                                  ? K_RTC
                      : hw_strneq(u->ports[k], "nvme.", 5) || hw_strneq(u->ports[k], "sd.", 3) ? K_BLOCK
                      : hw_strneq(u->ports[k], "con.", 4)                                  ? K_CON
                                                                                               : K_RNG;
            c->unit = u;
            nclients++;
            any = true;
        }
        if (any && nlive < sizeof live / sizeof live[0])
            live[nlive++] = u;
    }
    if (!nlive) {
        say("hwdb: kill test: no driver here serves a device; nothing to kill");
        phase = T_POWER;
        return;
    }
    phase = T_BENCH;
}

/* Each block device measured in turn, then the clients started. */
static void bench_step(void)
{
    struct ProcessInfo pi;

    if (bench) {
        if (QueryProcess(bench, &pi) != 0 || pi.pi_State < PROCS_EXITED)
            return;
        if (pi.pi_State != PROCS_EXITED || pi.pi_ExitCode != 0)
            fail("blkbench failed");
        else
            measured++;
        CloseHandle(bench);
        bench = 0;
    }
    for (; benched < nclients; benched++) {
        const struct client *c = &clients[benched];
        uint64_t unit = 0;
        int64_t p;

        const char *d = c->port;

        if (c->kind != K_BLOCK)
            continue;
        while (*d && *d != '.')
            d++;
        for (d++; *d >= '0' && *d <= '9'; d++)
            unit = unit * 10 + (uint64_t)(*d - '0');
        if (c->port[0] == 's')
            unit |= 1u << 8;            /* "sd.N", not "nvme.N" */
        if ((p = CreateProcess("blkbench", unit, SIG_DEAD, 0)) < 0) {
            fail("blkbench couldn't be started");
            continue;
        }
        bench = (Handle)p;
        benched++;
        phase_at = hwdb.now;
        return;
    }

    phase = T_DOS;
}

static void start_clients(void)
{
    handles = own_handles();
    for (unsigned i = 0; i < nclients; i++)
        if (CreateTask(client, i) < 0)      /* its handle goes when it ends */
            fail("a client task couldn't be made");
    rounds = ROUNDS * nlive;
    wake = hwdb.now + 20 * MS;      /* let the clients get going */
    phase = T_ROUNDS;
}

/* With block devices: dos, and dostest against it. */
static void dos_step(void)
{
    struct ProcessInfo pi;
    int64_t p;
    bool any = false;

    for (unsigned i = 0; i < nclients; i++)
        any |= clients[i].kind == K_BLOCK;
    if (!any) {
        start_clients();
        return;
    }
    if (!dos) {
        if ((p = CreateProcess("dos", 0, SIG_DEAD, 0)) < 0 ||
            (bench = (Handle)CreateProcess("dostest", 0, SIG_DEAD, 0)) == 0 || (int64_t)bench < 0) {
            fail("dos or dostest couldn't be started");
            bench = 0;
            start_clients();
            return;
        }
        dos = (Handle)p;
        phase_at = hwdb.now;
        return;
    }
    if (QueryProcess(bench, &pi) != 0 || pi.pi_State < PROCS_EXITED) {
        if (hwdb.now - phase_at < 15000 * MS)
            return;
        fail("dostest didn't finish");
        KillProcess(bench);
        return;
    }
    if (pi.pi_State != PROCS_EXITED || pi.pi_ExitCode != 0)
        fail("dostest failed");
    else
        files_ok = true;
    CloseHandle(bench);
    bench = 0;
    phase = T_SHELL;
}

/* The Shell, loaded from the disk, on its test script. */
static void shell_step(void)
{
    static struct dos d;
    struct ProcessInfo pi;

    if (!bench) {
        int64_t p;

        if (dos_open(&d, SIG_SLEEP + 1) != 0 ||
            (p = dos_run(&d, "C:shell", "-s SYS:EFI/planar/S/test", SIG_DEAD, 0)) < 0) {
            fail("the Shell couldn't be loaded");
            dos_close(&d);
            start_clients();
            return;
        }
        dos_close(&d);
        bench = (Handle)p;
        phase_at = hwdb.now;
        return;
    }
    if (QueryProcess(bench, &pi) != 0 || pi.pi_State < PROCS_EXITED) {
        if (hwdb.now - phase_at < 15000 * MS)
            return;
        fail("the Shell's script didn't finish");
        KillProcess(bench);
        return;
    }
    if (pi.pi_State != PROCS_EXITED || pi.pi_ExitCode != 0)
        fail("a line of the Shell's script failed");
    else
        shell_ok = true;
    CloseHandle(bench);
    bench = 0;
    start_clients();
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

/* What V4's steps 5 to 7 saw here, for the kernel's "V4.n done" lines. */
static void step_lines(void)
{
    unsigned blocks = 0, kills = 0, cons = 0;
    struct hw_line l = { 0 };

    for (unsigned i = 0; i < nclients; i++) {
        blocks += clients[i].kind == K_BLOCK;
        cons += clients[i].kind == K_CON;
        if (clients[i].kind == K_BLOCK)
            kills += clients[i].deaths;
    }
    if (!blocks) {
        say("V4.5 storage: no block device on this machine");
        say("V4.6 files: no block device on this machine");
        say("V4.7 shell: no block device on this machine");
        return;
    }
    hw_put(&l, "V4.5 storage ok: ");
    hw_put_dec(&l, measured);
    hw_put(&l, measured == 1 ? " block device" : " block devices");
    hw_put(&l, " measured at depths 1, 2, 8 and 32; their clients recovered from ");
    hw_put_dec(&l, kills);
    hw_put(&l, " driver deaths under I/O");
    say(l.text);
    if (files_ok)
        say("V4.6 files ok: FAT32 under dos, files read, written and removed where it may, "
            "a program loaded from the disk and run");
    if (shell_ok)
        say(cons ? "V4.7 shell ok: the Shell from the disk ran its script there, on the framebuffer"
                 : "V4.7 shell ok: the Shell from the disk ran its script there (no framebuffer: "
                   "to the log)");
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
    /* A restart may take a while (a big screen cleared): each client is
     * given as long as any restart may take to come back from its last. */
    for (unsigned i = 0; i < nclients; i++)
        if (!clients[i].stop && clients[i].recovered != clients[i].deaths &&
            hwdb.now < phase_at + 3000 * MS)
            return;
    for (unsigned i = 0; i < nclients; i++)
        clients[i].stop = true;
    for (unsigned i = 0; i < nclients; i++)
        all &= clients[i].done;
    if (!all)
        return;
    /* A client says it's done just before it ends, and its handle goes
     * as it ends: give the last a moment to. */
    if (!all_done_at)
        all_done_at = hwdb.now;
    if (own_handles() != handles && hwdb.now < all_done_at + 250 * MS)
        return;
    for (unsigned i = 0; i < nclients; i++) {
        const struct client *c = &clients[i];

        answers += c->answers;
        deaths += c->deaths;
        if (c->answers == 0 || c->deaths == 0)
            fail("a client saw no answers, or no deaths");
        if (c->recovered != c->deaths)
            fail("a client didn't recover from every death");
        if (c->bad) {
            struct hw_line l = { 0 };

            hw_put(&l, "the client of ");
            hw_put(&l, c->port);
            hw_put(&l, " had ");
            hw_put_dec(&l, c->bad);
            hw_put(&l, " wrong answers, or no driver for 3 s; the last ");
            if (c->last_bad < 0)
                hw_put(&l, "-");
            hw_put_dec(&l, c->last_bad < 0 ? (uint64_t)-c->last_bad : (uint64_t)c->last_bad);
            fail(l.text);
        }
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
    step_lines();
    phase = T_POWER;
}

static void stop_all(void)
{
    bool any = false;

    if (phase == T_STOP) {
        if (dos)
            CloseHandle(dos);       /* which ends it, before its devices */
        dos = 0;
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
    case T_BENCH:
        bench_step();
        break;
    case T_DOS:
        dos_step();
        break;
    case T_SHELL:
        shell_step();
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
    case T_POWER:
        if (power_test())
            phase = T_DISPLAY;
        break;
    case T_DISPLAY:
        if (display_test())
            phase = T_INPUT;
        break;
    case T_INPUT:
        if (input_test())
            phase = T_GPU;
        break;
    case T_GPU:
        if (gpu_test())
            phase = T_STOP;
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
    case T_BENCH:
    case T_DOS:
    case T_SHELL:
    case T_RETRY:
    case T_DRAIN:
    case T_POWER:
    case T_DISPLAY:
    case T_INPUT:
    case T_GPU:
        return hwdb.now + 5 * MS;
    default:
        return 0;
    }
}
