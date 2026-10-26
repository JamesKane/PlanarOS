/*
 * servers/hwdb/power.c -- power before lease (§11.5 D3, v5-plan step 2)
 *
 * A unit whose devices name clocks, resets or power domains that power
 * provides is voted for before its leases are granted: hwdb sends power
 * those references, tagged with the unit's number, and starts the unit
 * when the answer comes. hwdb never waits for it (D6): the unit is
 * U_POWERING meanwhile. When the unit is torn down, after its leases are
 * revoked, the vote is taken back; the restart votes again.
 *
 * hwdb's one connection to power is one of server.c's, marked as
 * power's. If power dies, its votes die with it, and so does the
 * connection: once it's running again hwdb connects again and votes for
 * every unit it had voted for, and for any still waiting.
 *
 * In test mode on a machine with nothing to power (QEMU's), hwdb adds
 * power's model (drv/power/model.c) and has the real-time clock (or,
 * under edk2, which keeps the RTC, the display) need its
 * domain 1, so the whole path runs there: the kill test's deaths take
 * that driver's vote away and its restarts bring it back.
 */
#include <planar/devices/power.h>
#include <planar/syscalls.h>

#include "../../drv/power/smt.h"
#include "server.h"

void *memset(void *dst, int c, size_t n);

constexpr unsigned MSGS = 64;             /* a power of two: they're the ring's slots too */

struct power power;

static struct PowerMsg *pool[MSGS];
static unsigned npool, made;

/* The connection, while power is up: server.c's array moves it about. */
static struct MsgPort *port(void)
{
    return power.up ? server_conn(CONN_POWER) : nullptr;
}

static bool is_power(const struct hw_graph *g, hw_id s)
{
    const struct hw_node *n = &g->nodes[s];

    return s != HW_NONE && n->driver != HW_NODRV && hw_streq(g->drivers[n->driver].name, "power") &&
           n->lease != HW_NOLEASE;
}

/* The unit's references to power's devices, as power wants them; the
 * count, or -1 if there are more than a message holds. */
static int refs_of(const struct unit *u, struct PowerRef *out)
{
    const struct hw_graph *g = &hwdb.g;
    const struct hw_lease *ls = &g->leases[u - hwdb.units];
    int n = 0;

    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *d = &g->nodes[ls->nodes[i]];

        for (unsigned k = 0; k < d->nrefs; k++) {
            const struct hw_ref *r = &d->refs[k];
            const struct hw_lease *pl;
            uint32_t kind = r->kind == HW_REF_CLOCK ? POWER_CLOCK : r->kind == HW_REF_RESET ? POWER_RESET
                          : r->kind == HW_REF_POWER ? POWER_DOMAIN : 0;
            unsigned dev = 0;

            if (!kind || r->dropped || !is_power(g, r->supplier))
                continue;
            pl = &g->leases[g->nodes[r->supplier].lease];
            while (dev < pl->nnodes && pl->nodes[dev] != r->supplier)
                dev++;
            if (dev == pl->nnodes)
                continue;
            if (n == (int)POWER_REFS)
                return -1;
            if (out) {
                out[n] = (struct PowerRef){ .pr_Kind = kind, .pr_Device = dev,
                                            .pr_NCells = r->ncells < 4 ? r->ncells : 4 };
                for (unsigned c = 0; c < out[n].pr_NCells; c++)
                    out[n].pr_Cells[c] = r->cells[c];
            }
            n++;
        }
    }
    return n;
}

/* A named set a quirk gave one of the unit's nodes, or nullptr. */
static const char *set_of(const struct unit *u)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];

    for (unsigned i = 0; i < ls->nnodes; i++)
        if (hwdb.g.nodes[ls->nodes[i]].power_set)
            return hwdb.g.nodes[ls->nodes[i]].power_set;
    return nullptr;
}

/* The vote's body: the named set, or the references; false if they
 * don't fit a message. */
static bool fill(const struct unit *u, struct PowerMsg *m)
{
    const char *set = set_of(u);

    if (set) {
        for (unsigned i = 0; set[i] && i < POWER_NAME - 1; i++)
            m->pm_Name[i] = set[i];
        return true;
    }
    return (int)(m->pm_Count = (uint32_t)refs_of(u, m->pm_Ref)) >= 0;
}

bool power_needed(const struct unit *u)
{
    return !hw_streq(unit_driver(u), "power") && (set_of(u) || refs_of(u, nullptr) != 0);
}

static struct PowerMsg *msg(void)
{
    struct PowerMsg *m;

    if (npool)
        return pool[--npool];
    if (made == MSGS || !(m = AllocMsg(port(), sizeof *m)))
        return nullptr;
    made++;
    return m;
}

static bool send(struct PowerMsg *m)
{
    m->pm_Msg.mn_Length = sizeof *m;
    if (PutMsg(port(), &m->pm_Msg) != 0) {
        pool[npool++] = m;
        return false;
    }
    return true;
}

/* Ask for u's vote: U_POWERING until the answer. */
static void ask(struct unit *u)
{
    struct PowerMsg *m;

    u->state = U_POWERING;
    u->asked = false;
    if (!power.up || !(m = msg()))
        return;                     /* sent when power is back */
    memset(m, 0, sizeof *m);
    m->pm_Op = POWER_ON;
    m->pm_Tag = (uint32_t)(u - hwdb.units);
    if (!fill(u, m)) {
        pool[npool++] = m;
        unit_fail(u, "it names more power references than a vote holds");
        return;
    }
    u->asked = send(m);
    u->due = hwdb.now + POWER_DEADLINE_MS * MS;    /* the answer's deadline (server.c) */
}

void power_vote(struct unit *u)
{
    ask(u);
}

void power_release(struct unit *u)
{
    struct PowerMsg *m;

    if (!u->voted)
        return;
    u->voted = false;
    if (!power.up || !(m = msg()))
        return;                     /* power went: the vote went with it */
    memset(m, 0, sizeof *m);
    m->pm_Op = POWER_OFF;
    m->pm_Tag = (uint32_t)(u - hwdb.units);
    send(m);
}

/* A vote again for a unit that's running: power came back. */
static void revote(struct unit *u)
{
    struct PowerMsg *m;

    if (!(m = msg()))
        return;
    memset(m, 0, sizeof *m);
    m->pm_Op = POWER_ON;
    m->pm_Tag = (uint32_t)(u - hwdb.units);
    fill(u, m);
    u->revoting = send(m);
}

int64_t power_query(const char *name, uint32_t tag)
{
    struct PowerMsg *m;

    if (!power.up || !(m = msg()))
        return ERR_NOTFOUND;
    memset(m, 0, sizeof *m);
    m->pm_Op = POWER_QUERY;
    m->pm_Tag = tag;
    for (unsigned i = 0; name[i] && i < POWER_NAME - 1; i++)
        m->pm_Name[i] = name[i];
    return send(m) ? 0 : ERR_FULL;
}

int64_t power_vote_set(const char *name, uint32_t tag, bool on)
{
    struct PowerMsg *m;

    if (!power.up || !(m = msg()))
        return ERR_NOTFOUND;
    memset(m, 0, sizeof *m);
    m->pm_Op = on ? POWER_ON : POWER_OFF;
    m->pm_Tag = tag;
    for (unsigned i = 0; name[i] && i < POWER_NAME - 1; i++)
        m->pm_Name[i] = name[i];
    return send(m) ? 0 : ERR_FULL;
}

/* An answer. Tags below the unit count are units'; the rest the test's. */
static void answer(const struct PowerMsg *m)
{
    uint32_t tag = m->pm_Tag, op = m->pm_Op;
    int32_t err = m->pm_Msg.mn_Error;
    struct unit *u;

    if (tag >= hwdb.g.nleases) {
        test_power_answer(m);
        return;
    }
    u = &hwdb.units[tag];
    if (op != POWER_ON)
        return;
    if (u->revoting) {
        u->revoting = false;
        if (err == 0)
            u->voted = true;
        else
            check_failed(u, "power refused its vote again, after power restarted");
        return;
    }
    if (u->state != U_POWERING || !u->asked)
        return;                     /* a stale answer, from before a power death */
    u->asked = false;
    if (err) {
        unit_fail(u, "power couldn't turn its clocks, resets or domains on");
        return;
    }
    u->voted = true;
    /* Its votes went with its last death: whatever they turned on was off
     * again, and comes on again now. */
    if (hwdb.test && u->powered && m->pm_TurnedOn != u->turned_on)
        check_failed(u, "its last vote's resources weren't turned off at its death");
    else if (u->powered && u->turned_on)
        power.checked++;
    if (!u->powered) {
        u->powered = true;
        u->turned_on = m->pm_TurnedOn;
    }
    power.votes++;
    unit_launch(u);
}

void power_serve(struct MsgPort *mp)
{
    struct PowerMsg *m;

    while ((m = (struct PowerMsg *)GetMsgSized(mp, sizeof *m))) {
        struct PowerMsg copy = *m;          /* power's memory, too */

        pool[npool++] = m;
        answer(&copy);
    }
}

/* power's unit is running: connect, and vote for everyone owed one. */
void power_up(void)
{
    struct MsgPort mp;

    if (power.up)
        return;
    if (ConnectPort(&mp, "power", SIG_CONN, MSGS, MSGS * sizeof(struct PowerMsg) + 4096) != 0) {
        say("hwdb: power started, but its port wouldn't connect");
        return;
    }
    npool = made = 0;
    if (!server_add_conn(&mp, CONN_POWER)) {
        ClosePort(&mp);
        return;
    }
    power.up = true;
    power.ups++;
    for (uint32_t i = 0; i < hwdb.g.nleases; i++) {
        struct unit *u = &hwdb.units[i];

        if (u->state == U_POWERING && !u->asked)
            ask(u);
        else if (u->voted)
            revote(u);
    }
}

/* power's connection went (it died): its votes did too. */
void power_down(void)
{
    power.up = false;
    power.downs++;
    for (uint32_t i = 0; i < hwdb.g.nleases; i++) {
        struct unit *u = &hwdb.units[i];

        u->asked = u->revoting = false;
        u->powered = false;         /* what power keeps on now is no measure */
    }
}

/* --- Test mode on QEMU: the model. --- */

static bool has_key(const char *key)
{
    for (hw_id i = 1; i < hwdb.g.count; i++)
        if (hw_has_key(&hwdb.g.nodes[i], key))
            return true;
    return false;
}

void power_model(void)
{
    static const char *const real[] = { "ky,x1-pm-domain", "CIXHA004" };
    /* The RTC, else (edk2 keeps it for itself) GOP's framebuffer: each a
     * driver the kill test kills. */
    static const char *const users[] = { "google,goldfish-rtc", "arm,pl031", "ARMH0031",
                                         "simple-framebuffer" };
    struct hw_graph *g = &hwdb.g;
    hw_id m;

    for (unsigned i = 0; i < sizeof real / sizeof real[0]; i++)
        if (has_key(real[i]))
            return;
    if ((m = hw_node_new(g, 0, "power-model")) == HW_NONE || !hw_key_add(g, m, "planar,power-model"))
        return;
    g->nodes[m].enabled = true;
    g->nodes[m].bare = true;
    power.model = true;
    for (unsigned k = 0; k < sizeof users / sizeof users[0] && !power.consumer; k++)
        for (hw_id i = 1; i < g->count && !power.consumer; i++)
            if (g->nodes[i].enabled && hw_has_key(&g->nodes[i], users[k])) {
                struct hw_ref *r = hw_ref_add(g, i, HW_REF_POWER, "power-domains", true);

                if (!r)
                    return;
                r->target = r->supplier = m;
                r->ncells = 1;
                r->cells[0] = 1;
                power.consumer = true;
            }
}

/* --- The kill test's part (killtest.c's T_POWER). --- */

constexpr uint32_t TAG_QUERY = 0x10000, TAG_BROKEN = 0x10001;

enum : uint8_t { P_START, P_BEFORE, P_VOTED, P_KILLED, P_BROKEN, P_RESTART, P_DONE };

static struct {
    uint8_t step;
    bool waiting;                   /* for an answer */
    bool answered;
    struct PowerMsg got;
    uint32_t before_on, total, kept;
    uint64_t at;
    Handle voter;
    unsigned ups;
    bool broken_ok;
} pt;

void test_power_answer(const struct PowerMsg *m)
{
    pt.got = *m;
    pt.answered = true;
    pt.waiting = false;
}

static void test_fail(const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: power test: FAILED: ");
    hw_put(&l, what);
    say(l.text);
    hwdb.errors++;
    pt.step = P_DONE;
}

/* Ask about "gpu" unless an answer's awaited; true once one has come. */
static bool asked_gpu(void)
{
    if (pt.answered) {
        pt.answered = false;
        return true;
    }
    if (!pt.waiting && power_query("gpu", TAG_QUERY) == 0)
        pt.waiting = true;
    return false;
}

static bool late(uint64_t ms)
{
    return hwdb.now - pt.at > ms * MS;
}

/*
 * The stop method. power has no DMA, but a power killed mid-call leaves
 * its SCMI channel busy for whoever uses it next (smt.h): on the CIX's
 * SHM0, its first window is mailbox 6's channel, rung by a doorbell, and
 * the other TF-A's, rung by an SMC. A channel rung is given power's own
 * wait, 300 ms, to be answered. With no DMA, it's stopped either way.
 */
bool stop_power(struct unit *u)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS) {
            say_unit(u, "its registers couldn't be mapped: its channels left as they are");
            return true;
        }
    }
    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *n = &hwdb.g.nodes[ls->nodes[i]];
        bool first = true;

        if (!hw_has_key(n, "CIXHA004"))
            continue;
        for (unsigned k = 0; k < n->nres; k++) {
            if (n->res[k].kind != HW_MMIO || n->res[k].size < SMT_SIZE)
                continue;
            if (smt_release((volatile uint8_t *)n->res[k].base, !first, CurrentTime,
                            CurrentTime() + 300 * MS))
                say_unit(u, first ? "it left mailbox 6's channel busy: freed"
                                  : "it left the SMC channel busy: freed");
            first = false;
        }
    }
    return true;
}

struct unit *power_unit(void)
{
    for (uint32_t i = 0; i < hwdb.g.nleases; i++)
        if (hw_streq(unit_driver(&hwdb.units[i]), "power") && hwdb.units[i].state != U_DONE)
            return &hwdb.units[i];
    return nullptr;
}

static unsigned powered_units(bool *all_voted)
{
    unsigned n = 0;

    *all_voted = true;
    for (uint32_t i = 0; i < hwdb.g.nleases; i++) {
        const struct unit *u = &hwdb.units[i];

        if (u->state != U_RUNNING || !power_needed(u))
            continue;
        n++;
        *all_voted &= u->voted && !u->revoting;
    }
    return n;
}

static void lines(void)
{
    struct hw_line l = { 0 };
    bool all;
    unsigned n = powered_units(&all);

    hw_put(&l, "V5.2 power ok: ");
    hw_put_dec(&l, n);
    hw_put(&l, n == 1 ? " driver voted for before its lease, " : " drivers voted for before their leases, ");
    hw_put_dec(&l, power.votes);
    hw_put(&l, " votes; ");
    hw_put_dec(&l, power.checked);
    hw_put(&l, " restarts after a death turned its resources on again");
    if (power.model)
        hw_put(&l, " (QEMU's model)");
    say(l.text);
    l = (struct hw_line){ 0 };
    hw_put(&l, "V5.2 power ok: a client's vote turned \"gpu\" on (");
    hw_put_dec(&l, pt.total - pt.before_on);
    hw_put(&l, " of ");
    hw_put_dec(&l, pt.total);
    hw_put(&l, pt.kept ? ", the rest the firmware's) and its death off" : ") and its death off");
    if (pt.broken_ok)
        hw_put(&l, "; one that can't be met refused");
    hw_put(&l, "; power killed and the votes made again");
    say(l.text);
}

/* One step; true when the test is over. */
bool power_test(void)
{
    struct ProcessInfo pi;
    struct unit *pu;
    int64_t p;

    switch (pt.step) {
    case P_START:
        if (!power_unit()) {
            say("V5.2 power: not exercised: nothing here provides power");
            return true;
        }
        if (!power.up) {
            test_fail("power isn't running");
            return true;
        }
        if (power.model && !power.consumer) {
            test_fail("the model has no driver to power");
            return true;
        }
        pt.at = hwdb.now;
        pt.step = P_BEFORE;
        [[fallthrough]];
    case P_BEFORE:
        if (!asked_gpu()) {
            if (late(3000))
                test_fail("power didn't answer a query");
            return false;
        }
        if (pt.got.pm_Msg.mn_Error) {
            test_fail("power has no \"gpu\" set");
            return true;
        }
        pt.before_on = pt.got.pm_On;
        pt.total = pt.got.pm_Total;
        pt.kept = pt.got.pm_Kept;
        if ((p = CreateProcess("powervote", 0, SIG_DEAD, 0)) < 0) {
            test_fail("powervote couldn't be started");
            return true;
        }
        pt.voter = (Handle)p;
        pt.at = hwdb.now;
        pt.step = P_VOTED;
        return false;
    case P_VOTED:
        if (QueryProcess(pt.voter, &pi) == 0 && pi.pi_State >= PROCS_EXITED) {
            test_fail("powervote ended before it was killed (its checks failed)");
            return true;
        }
        if (!asked_gpu()) {
            if (late(3000))
                test_fail("a client's vote didn't turn \"gpu\" on");
            return false;
        }
        if (pt.got.pm_On != pt.total)
            return false;           /* not voted yet: ask again */
        KillProcess(pt.voter);
        pt.at = hwdb.now;
        pt.step = P_KILLED;
        return false;
    case P_KILLED:
        if (QueryProcess(pt.voter, &pi) != 0 || pi.pi_State < PROCS_EXITED)
            return false;
        if (!asked_gpu()) {
            if (late(3000))
                test_fail("a dead client's vote wasn't taken back: \"gpu\" stayed on");
            return false;
        }
        if (pt.got.pm_On != pt.before_on)
            return false;
        CloseHandle(pt.voter);
        pt.voter = 0;
        pt.at = hwdb.now;
        pt.step = P_BROKEN;
        if (power.model && power_vote_set("broken", TAG_BROKEN, true) == 0)
            pt.waiting = true;
        return false;
    case P_BROKEN:
        if (power.model) {
            if (!pt.answered) {
                if (late(3000))
                    test_fail("power didn't answer the failing vote");
                return false;
            }
            pt.answered = false;
            if (pt.got.pm_Msg.mn_Error != ERR_TIMEOUT) {
                test_fail("a vote for a domain that never comes on didn't fail");
                return true;
            }
            pt.broken_ok = true;
        }
        if (!(pu = power_unit()) || pu->state != U_RUNNING) {
            test_fail("power wasn't running to be killed");
            return true;
        }
        pt.ups = power.ups;
        unit_kill(pu);
        pt.at = hwdb.now;
        pt.step = P_RESTART;
        return false;
    case P_RESTART: {
        bool all;

        powered_units(&all);
        if (power.ups == pt.ups || !all) {
            if (late(5000))
                test_fail("after power restarted, a driver wasn't voted for again");
            return false;
        }
        if (power.model && !power.checked) {
            test_fail("the model's driver was never restarted with its vote taken back");
            return true;
        }
        lines();
        pt.step = P_DONE;
        return true;
    }
    default:
        return true;
    }
}
