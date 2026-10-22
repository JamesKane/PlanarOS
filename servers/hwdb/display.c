/*
 * servers/hwdb/display.c -- display's stop method and test (v5-plan step 3)
 *
 * display's controller can't be stopped as the others are, by a reset:
 * the screen would go dark, and the CIX's DisplayPort link with it for
 * good. What a dead display's device may still read is its planes'
 * buffers (held for it by the kernel: area_hold_lent), so its stop is
 * to turn every plane off but the one on the firmware's framebuffer,
 * and wait for the latch; then its memory can go back, and the console
 * stays on the screen. komeda.h and saturn.h have the registers, shared
 * with display. fb.c's planes are composed by display itself: nothing
 * else reads them.
 *
 * The test (killtest.c's T_DISPLAY), with displaytest as the client:
 * its planes shown, then it killed, and its buffers freed; then planes
 * shown again and display killed under them, the client finding them
 * still its to write and display back without them. hwdb watches the
 * buffers with DISPLAY_INFO on a connection of its own.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/syscalls.h>

#include "../../drv/display/komeda.h"
#include "../../drv/display/saturn.h"
#include "server.h"

void *memset(void *dst, int c, size_t n);

constexpr uint64_t SPINS = 20000000;

/* --- The stop method. --- */

static const struct hw_res *first_mmio(const struct hw_node *n)
{
    for (unsigned k = 0; k < n->nres; k++)
        if (n->res[k].kind == HW_MMIO && n->res[k].size)
            return &n->res[k];
    return nullptr;
}

bool stop_display(struct unit *u)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];
    const struct hw_res *fb = nullptr;

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS)
            return false;
    }
    for (unsigned i = 0; i < ls->nnodes; i++)
        if (hw_has_key(&hwdb.g.nodes[ls->nodes[i]], "simple-framebuffer"))
            fb = first_mmio(&hwdb.g.nodes[ls->nodes[i]]);
    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *n = &hwdb.g.nodes[ls->nodes[i]];
        const struct hw_res *r = first_mmio(n);
        volatile uint8_t *regs;

        if (!r)
            continue;
        regs = (volatile uint8_t *)r->base;
        if (hw_has_key(n, "CIXH5010") && r->size >= 0x2000 &&
            !komeda_quiet(regs, fb ? fb->base : 0, fb ? fb->base + fb->size : 0, SPINS))
            return false;
        if (hw_has_key(n, "ky,dpu") && r->size >= 0x1a000 && !saturn_quiet(regs, SPINS))
            return false;
    }
    return true;
}

/* --- The connection: test mode only. --- */

constexpr uint32_t MSGS = 4;

static struct {
    bool up;
    struct DisplayMsg *m;
    bool waiting, answered;
    uint32_t buffers;               /* the last answer's */
} dc;

void display_up(void)
{
    struct MsgPort mp;

    if (!hwdb.test || dc.up)
        return;
    if (ConnectPort(&mp, "display.0", SIG_CONN, MSGS, MSGS * sizeof *dc.m + 4096) != 0)
        return;
    if (!server_add_conn(&mp, CONN_DISPLAY)) {
        ClosePort(&mp);
        return;
    }
    dc = (typeof(dc)){ .up = true, .m = AllocMsg(server_conn(CONN_DISPLAY), sizeof *dc.m) };
}

void display_down(void)
{
    dc.up = false;
}

void display_serve(struct MsgPort *mp)
{
    struct DisplayMsg *m;

    while ((m = (struct DisplayMsg *)GetMsgSized(mp, sizeof *m))) {
        dc.buffers = m->dm_Msg.mn_Error == 0 ? m->dm_Buffers : UINT32_MAX;
        dc.answered = true;
        dc.waiting = false;
    }
}

/* Ask how many buffers display holds unless an answer's awaited; true
 * once one has come (in dc.buffers). */
static bool asked(void)
{
    if (dc.answered) {
        dc.answered = false;
        return true;
    }
    if (!dc.up || dc.waiting || !dc.m)
        return false;
    memset(dc.m, 0, sizeof *dc.m);
    dc.m->dm_Msg.mn_Length = sizeof *dc.m;
    dc.m->dm_Op = DISPLAY_INFO;
    dc.m->dm_Task = FindTask(nullptr);
    if (PutMsg(server_conn(CONN_DISPLAY), &dc.m->dm_Msg) == 0)
        dc.waiting = true;
    return false;
}

/* --- The test. --- */

enum : uint8_t { D_START, D_SHOWN, D_KILLED, D_SHOWN2, D_BACK, D_DONE };

static struct {
    uint8_t step;
    uint64_t at;
    Handle client;
    unsigned deaths;
    uint64_t held;                  /* hwdb.quarantined before display's death */
    bool native;
} dt;

static void test_fail(const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: display test: FAILED: ");
    hw_put(&l, what);
    say(l.text);
    hwdb.errors++;
    dt.step = D_DONE;
}

static bool late(uint64_t ms)
{
    return hwdb.now - dt.at > ms * MS;
}

struct unit *display_unit(void)
{
    for (uint32_t i = 0; i < hwdb.g.nleases; i++)
        if (hw_streq(unit_driver(&hwdb.units[i]), "display") && hwdb.units[i].state == U_RUNNING)
            for (unsigned k = 0; k < hwdb.g.leases[i].nnodes; k++)
                if (hw_streq(hwdb.units[i].ports[k], "con.0"))
                    return &hwdb.units[i];
    return nullptr;
}

static bool client_ended(int64_t *code)
{
    struct ProcessInfo pi;

    if (QueryProcess(dt.client, &pi) != 0 || pi.pi_State < PROCS_EXITED)
        return false;
    *code = pi.pi_State == PROCS_EXITED ? pi.pi_ExitCode : -1;
    return true;
}

static bool start_client(uint64_t arg)
{
    int64_t p = CreateProcess("displaytest", arg, SIG_DEAD, 0);

    if (p < 0) {
        test_fail("displaytest couldn't be started");
        return false;
    }
    dt.client = (Handle)p;
    dt.at = hwdb.now;
    return true;
}

static void lines(void)
{
    struct hw_line l = { 0 };

    hw_put(&l, "V5.3 display ok: a client's overlay and cursor shown, flipped at vblank and freed at "
               "its death; display killed under them came back ");
    hw_put(&l, dt.native ? "on the firmware's mode, its console still up" : "with its console (QEMU's framebuffer)");
    say(l.text);
    l = (struct hw_line){ 0 };
    hw_put(&l, "V5.3 display ok: ");
    hw_put_dec(&l, hwdb.quarantined - dt.held);
    hw_put(&l, " pages of a dead display's buffers held for the client until its planes were off");
    say(l.text);
}

bool display_test(void)
{
    struct unit *u;
    int64_t code;

    switch (dt.step) {
    case D_START:
        if (!(u = display_unit())) {
            say("V5.3 display: not exercised: no screen here");
            return true;
        }
        if (!dc.up) {
            test_fail("display's port wouldn't connect");
            return true;
        }
        for (unsigned k = 0; k < hwdb.g.leases[u - hwdb.units].nnodes; k++)
            dt.native |= !hw_has_key(&hwdb.g.nodes[hwdb.g.leases[u - hwdb.units].nodes[k]],
                                     "simple-framebuffer");
        if (!start_client(0))
            return true;
        dt.step = D_SHOWN;
        return false;
    case D_SHOWN:
        if (client_ended(&code)) {
            test_fail("displaytest ended before it was killed (its checks failed)");
            return true;
        }
        if (!asked() || dc.buffers < 4) {
            if (late(15000))
                test_fail("a client's planes never came up");
            return false;
        }
        KillProcess(dt.client);
        dt.at = hwdb.now;
        dt.step = D_KILLED;
        return false;
    case D_KILLED:
        if (!client_ended(&code) || !asked() || dc.buffers != 0) {
            if (late(3000))
                test_fail("a dead client's buffers weren't freed");
            return false;
        }
        CloseHandle(dt.client);
        if (!start_client(1))
            return true;
        dt.step = D_SHOWN2;
        return false;
    case D_SHOWN2:
        if (client_ended(&code)) {
            test_fail("displaytest ended before display was killed");
            return true;
        }
        if (!asked() || dc.buffers < 3) {
            if (late(10000))
                test_fail("the second client's planes never came up");
            return false;
        }
        if (!(u = display_unit())) {
            test_fail("display wasn't running to be killed");
            return true;
        }
        dt.deaths = u->deaths;
        dt.held = hwdb.quarantined;
        unit_kill(u);
        dt.at = hwdb.now;
        dt.step = D_BACK;
        return false;
    case D_BACK:
        if (!client_ended(&code)) {
            if (late(20000))
                test_fail("displaytest didn't see display come back");
            return false;
        }
        CloseHandle(dt.client);
        dt.client = 0;
        if (code != 0) {
            test_fail("displaytest's checks across display's death failed");
            return true;
        }
        if (!(u = display_unit()) || u->deaths == dt.deaths) {
            test_fail("display didn't come back");
            return true;
        }
        lines();
        dt.step = D_DONE;
        return true;
    default:
        return true;
    }
}
