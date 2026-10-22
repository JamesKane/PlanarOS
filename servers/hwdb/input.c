/*
 * servers/hwdb/input.c -- input, kept going; xhci's stop method; and the
 * input test (v5-plan step 4)
 *
 * input isn't a driver: it has no device. hwdb starts it before the
 * drivers and again whenever it ends, in both modes; drivers connect to
 * it when it's there (planar/devices/input.h).
 *
 * A dead xhci's controllers may still be reading its rings, and writing
 * its event ring and its keyboards' reports: its stop is to halt each
 * (xHCI 1.2 §5.4.1, USBCMD.R/S = 0 until USBSTS.HCH), after which the
 * controller touches no memory.
 *
 * The test (killtest.c's T_INPUT), with inputtest as the client, which
 * asks for keys and the mouse ("inputtest: send ..."); on QEMU the boot
 * check types them through QMP (tools/boot-check.py), and on a board
 * nobody does unless someone's there. Once with things as they are, once
 * after every xhci was killed and came back, and once across input's
 * own death. hwdb watches with INPUT_INFO on a connection of its own.
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/syscalls.h>

#include "server.h"

void *memset(void *dst, int c, size_t n);

/* --- input, kept going. --- */

static struct {
    Handle proc;
    uint64_t due;
    unsigned starts;
} ip;

void input_keep(void)
{
    struct ProcessInfo pi;
    int64_t p;

    if (hwdb.stopping) {
        if (ip.proc)
            CloseHandle(ip.proc);   /* which ends it */
        ip.proc = 0;
        return;
    }
    if (ip.proc && QueryProcess(ip.proc, &pi) == 0 && pi.pi_State >= PROCS_EXITED) {
        if (!hwdb.test)
            say("hwdb: input ended: restarting it");
        CloseHandle(ip.proc);
        ip.proc = 0;
        ip.due = hwdb.now + 10 * MS;
    }
    if (ip.proc || hwdb.now < ip.due)
        return;
    if ((p = CreateProcess("input", 0, SIG_DEAD, 0)) < 0) {
        say("hwdb: input couldn't be started");
        ip.due = hwdb.now + 1000 * MS;
        return;
    }
    ip.proc = (Handle)p;
    ip.starts++;
}

uint64_t input_due(void)
{
    return ip.proc ? 0 : ip.due;
}

unsigned input_kill(void)
{
    if (ip.proc)
        KillProcess(ip.proc);
    return ip.starts;
}

unsigned input_starts(void)
{
    return ip.starts;
}

/* --- xhci's stop. --- */

bool stop_xhci(struct unit *u)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS)
            return false;
    }
    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *n = &hwdb.g.nodes[ls->nodes[i]];
        volatile uint32_t *cap, *op;
        uint64_t until = CurrentTime() + 100 * MS;

        if (n->nres == 0 || n->res[0].kind != HW_MMIO || n->res[0].size < 0x1000)
            continue;
        cap = (volatile uint32_t *)n->res[0].base;
        if (cap[0] == ~0u || (cap[0] & 0xff) < 0x20)
            continue;               /* not an xHCI's (a DWC3 not yet a host) */
        op = (volatile uint32_t *)(n->res[0].base + (cap[0] & 0xff));
        op[0] &= ~1u;
        while (!(op[1] & 1))
            if (op[1] == ~0u || CurrentTime() > until)
                return false;
    }
    return true;
}

/* --- The connection: test mode only. --- */


static struct {
    bool up, waiting, answered;
    struct InputMsg *m;
    struct InputMsg got;            /* the last answer */
} ic;

static void conn_up(void)
{
    struct MsgPort mp;

    if (ic.up)
        return;
    if (ConnectPort(&mp, "input", SIG_CONN, 4, 4 * sizeof *ic.m + 4096) != 0)
        return;
    if (!server_add_conn(&mp, CONN_INPUT)) {
        ClosePort(&mp);
        return;
    }
    ic = (typeof(ic)){ .up = true, .m = AllocMsg(server_conn(CONN_INPUT), sizeof *ic.m) };
}

void input_conn_down(void)
{
    ic.up = false;
}

void input_conn_serve(struct MsgPort *mp)
{
    struct InputMsg *m;

    while ((m = (struct InputMsg *)GetMsgSized(mp, sizeof *m))) {
        ic.got = *m;
        ic.answered = m->im_Msg.mn_Error == 0;
        ic.waiting = false;
    }
}

/* Ask input how things are unless an answer's awaited; true once one
 * has come (in ic.got). */
static bool asked(void)
{
    if (ic.answered) {
        ic.answered = false;
        return true;
    }
    conn_up();
    if (!ic.up || ic.waiting || !ic.m)
        return false;
    memset(ic.m, 0, sizeof *ic.m);
    ic.m->im_Msg.mn_Length = sizeof *ic.m;
    ic.m->im_Op = INPUT_INFO;
    if (PutMsg(server_conn(CONN_INPUT), &ic.m->im_Msg) == 0)
        ic.waiting = true;
    return false;
}

/* --- The test. --- */

enum : uint8_t { I_START, I_FOUND, I_FIRST, I_KILL_XHCI, I_SECOND, I_COUNT, I_THIRD,
                 I_INPUT_BACK, I_DONE };

/* inputtest's round: "Hi 5\n" and Shift, each down and up; then a move
 * and a click, if there's a mouse. */
constexpr unsigned KEY_EVENTS = 12, MOUSE_EVENTS = 3;

static struct {
    uint8_t step;
    uint64_t at;
    Handle client;
    unsigned keyboards, mice, xhci_deaths, input_starts;
    uint64_t events;
    bool typed;                     /* inputtest's rounds had keys come */
} it;

static void test_fail(const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: input test: FAILED: ");
    hw_put(&l, what);
    say(l.text);
    hwdb.errors++;
    if (it.client) {
        KillProcess(it.client);
        CloseHandle(it.client);
        it.client = 0;
    }
    it.step = I_DONE;
}

static bool late(uint64_t ms)
{
    return hwdb.now - it.at > ms * MS;
}

static unsigned xhci_units(unsigned *deaths)
{
    unsigned n = 0;

    *deaths = 0;
    for (uint32_t i = 0; i < hwdb.g.nleases; i++)
        if (hw_streq(unit_driver(&hwdb.units[i]), "xhci") && hwdb.units[i].state == U_RUNNING) {
            n++;
            *deaths += hwdb.units[i].deaths;
        }
    return n;
}

/* 0 running, 1 ended well, 2 ended without keys (nobody typed), -1 badly. */
static int client_state(void)
{
    struct ProcessInfo pi;

    if (QueryProcess(it.client, &pi) != 0 || pi.pi_State < PROCS_EXITED)
        return 0;
    CloseHandle(it.client);
    it.client = 0;
    if (pi.pi_State != PROCS_EXITED)
        return -1;
    return pi.pi_ExitCode == 0 ? 1 : pi.pi_ExitCode == 2 ? 2 : -1;
}

static bool start_client(uint64_t mode)
{
    int64_t p = CreateProcess("inputtest", mode, SIG_DEAD, 0);

    if (p < 0) {
        test_fail("inputtest couldn't be started");
        return false;
    }
    it.client = (Handle)p;
    it.at = hwdb.now;
    return true;
}

static void lines(void)
{
    struct hw_line l = { 0 };

    hw_put(&l, "V5.4 input ok: ");
    hw_put_dec(&l, it.keyboards);
    hw_put(&l, it.keyboards == 1 ? " keyboard and " : " keyboards and ");
    hw_put_dec(&l, it.mice);
    hw_put(&l, it.mice == 1 ? " mouse" : " mice");
    hw_put(&l, " found over USB; ");
    hw_put(&l, it.typed ? "keys typed and the mouse moved reached a client's ring, again after "
                          "xhci was killed and after input was"
                        : "typing not exercised (nobody typed), xhci and input killed and back");
    say(l.text);
}

/* After a kill: back, with its keyboard. */
static bool found_again(void)
{
    return asked() && ic.got.im_Keyboards >= it.keyboards && ic.got.im_Mice >= it.mice;
}

static bool round_done(int s)
{
    if (s == 0)
        return false;
    if (s < 0) {
        test_fail("inputtest's checks failed");
        return false;
    }
    it.typed = s == 1;
    return true;
}

bool input_test(void)
{
    unsigned deaths;
    int s;

    switch (it.step) {
    case I_START:
        if (!xhci_units(&deaths)) {
            say("V5.4 input: not exercised: no USB controller here");
            return true;
        }
        it.at = hwdb.now;
        it.step = I_FOUND;
        return false;
    case I_FOUND:
        /* Its keyboards and mice found: two answers alike, a second apart. */
        if (!asked())
            return false;
        if (ic.got.im_Keyboards == 0 || ic.got.im_Keyboards != it.keyboards ||
            ic.got.im_Mice != it.mice || !late(1000)) {
            if (ic.got.im_Keyboards != it.keyboards || ic.got.im_Mice != it.mice) {
                it.keyboards = ic.got.im_Keyboards;
                it.mice = ic.got.im_Mice;
                if (it.keyboards)
                    it.at = hwdb.now;
            }
            if (!it.keyboards && late(10000)) {
                say("V5.4 input: not exercised: no keyboard on USB");
                return true;
            }
            return false;
        }
        if (!start_client(0))
            return true;
        it.step = I_FIRST;
        return false;
    case I_FIRST:
        if (!round_done(s = client_state())) {
            if (!s && late(20000))
                test_fail("inputtest's first round didn't end");
            return it.step == I_DONE;
        }
        xhci_units(&it.xhci_deaths);
        for (uint32_t i = 0; i < hwdb.g.nleases; i++)
            if (hw_streq(unit_driver(&hwdb.units[i]), "xhci") && hwdb.units[i].state == U_RUNNING)
                unit_kill(&hwdb.units[i]);
        it.at = hwdb.now;
        it.step = I_KILL_XHCI;
        return false;
    case I_KILL_XHCI:
        /* Every xhci back, its devices found again. */
        if (xhci_units(&deaths) == 0 || deaths == it.xhci_deaths || hwdb_busy() || !found_again()) {
            if (late(15000))
                test_fail("xhci didn't come back with its keyboard");
            return it.step == I_DONE;
        }
        if (!start_client(1))
            return true;
        it.step = I_SECOND;
        return false;
    case I_SECOND:
        if (!round_done(s = client_state())) {
            if (!s && late(20000))
                test_fail("inputtest's round after xhci's death didn't end");
            return it.step == I_DONE;
        }
        it.step = I_COUNT;
        return false;
    case I_COUNT:
        if (!asked())               /* a fresh count, from before the next round */
            return false;
        it.events = ic.got.im_Events;
        if (!start_client(2))
            return true;
        it.step = I_THIRD;
        return false;
    case I_THIRD:
        /* A round in, input is killed under it. */
        if ((s = client_state()) != 0) {
            test_fail("inputtest ended before input was killed");
            return true;
        }
        if (!asked() || !ic.got.im_Held ||
            (it.typed &&
             ic.got.im_Events < it.events + KEY_EVENTS + (it.mice ? MOUSE_EVENTS : 0))) {
            if (late(20000))
                test_fail("inputtest's round before input's death didn't end");
            return it.step == I_DONE;
        }
        if (!it.typed && !late(6000))
            return false;           /* its round's wait for keys, over */
        it.input_starts = ip.starts;
        KillProcess(ip.proc);
        it.at = hwdb.now;
        it.step = I_INPUT_BACK;
        return false;
    case I_INPUT_BACK:
        if ((s = client_state()) == 0) {
            if (late(20000))
                test_fail("inputtest didn't see input come back");
            return it.step == I_DONE;
        }
        if (s < 0) {
            test_fail("inputtest's checks across input's death failed");
            return true;
        }
        if (ip.starts == it.input_starts) {
            test_fail("input didn't come back");
            return true;
        }
        lines();
        it.step = I_DONE;
        return true;
    default:
        return true;
    }
}
