/*
 * servers/hwdb/intuition.c -- intuition, kept going; and the window test
 * (v7-plan steps 3 to 5)
 *
 * intuition is started when it's wanted (decision 4: on demand, not at
 * every boot): by the window test now, by the Workbench in V8. Once it
 * is, hwdb keeps it as it keeps input, started again 10 ms after it
 * ends, later each time if it keeps ending at once, until it's no longer
 * wanted. It has no device: a dead intuition's planes are display's to
 * turn off, as any client's.
 *
 * The test (killtest.c's T_WINDOW), on a machine with a screen, with
 * conformance/window.c as the client. hwdb watches intuition with
 * INTUI_INFO on a connection of its own, and display's buffers with
 * DISPLAY_INFO on another (display.c).
 * 1. window.c's first round: windows composed, presented and read back,
 *    then worked with the mouse and keys it asks for (on QEMU, the boot
 *    check sends them through QMP), then kill-window: a client of its
 *    own with two windows killed under them. It ends 0, or NO_INPUT if
 *    nobody moved the mouse: a board with nobody at it. intuition's
 *    handles and pages must then be back where they were before it.
 * 2. Its second round, across the servers' deaths. Once it shows three
 *    windows (the third, its cue, opened once the other two are
 *    checked), intuition is killed. It must be started again, display
 *    must free the dead one's buffers, and the client open its windows
 *    on the new one; then display is killed, and intuition must take
 *    its buffers again from the new display; then input is killed. The
 *    client checks what it sees of each and ends 0.
 * Then intuition is no longer wanted, and display must free its buffers.
 */
#include <stddef.h>

#include <planar/intuition.h>
#include <planar/syscalls.h>

#include "server.h"

void *memset(void *dst, int c, size_t n);

/* --- intuition, kept going while it's wanted. --- */

static struct {
    bool wanted;
    Handle proc;
    uint64_t due, started, delay;
    unsigned starts;
} ik;

void intuition_keep(void)
{
    struct ProcessInfo pi;
    int64_t p;

    if (hwdb.stopping && ik.proc) {
        CloseHandle(ik.proc);       /* which ends it */
        ik.proc = 0;
    }
    if (ik.proc && QueryProcess(ik.proc, &pi) == 0 && pi.pi_State >= PROCS_EXITED) {
        if (ik.wanted && !hwdb.test)
            say("hwdb: intuition ended: restarting it");
        CloseHandle(ik.proc);
        ik.proc = 0;
        ik.delay = hwdb.now - ik.started < 1000 * MS && ik.delay ? ik.delay * 2 : 10 * MS;
        if (ik.delay > 1000 * MS)
            ik.delay = 1000 * MS;
        ik.due = hwdb.now + ik.delay;
    }
    if (!ik.wanted || hwdb.stopping || ik.proc || hwdb.now < ik.due)
        return;
    if ((p = CreateProcess("intuition", 0, SIG_DEAD, 0)) < 0) {
        say("hwdb: intuition couldn't be started");
        ik.due = hwdb.now + 1000 * MS;
        return;
    }
    ik.proc = (Handle)p;
    ik.started = hwdb.now;
    ik.starts++;
}

uint64_t intuition_due(void)
{
    return ik.wanted && !ik.proc ? ik.due : 0;
}

/* Wanted from now on, or not: then killed, and not started again. */
static void intuition_want(bool on)
{
    ik.wanted = on;
    if (!on && ik.proc)
        KillProcess(ik.proc);
    if (on)
        intuition_keep();
}

/* --- The connection: test mode only. --- */

static struct {
    bool up, waiting, answered;
    struct IntuiMsg *m;
    struct IntuiMsg got;            /* the last answer */
} icn;

static void conn_up(void)
{
    struct MsgPort mp;

    if (icn.up)
        return;
    if (ConnectPort(&mp, "intuition", SIG_CONN, 2, 2 * sizeof *icn.m) != 0)
        return;
    if (!server_add_conn(&mp, CONN_INTUITION)) {
        ClosePort(&mp);
        return;
    }
    icn = (typeof(icn)){ .up = true, .m = AllocMsg(server_conn(CONN_INTUITION), sizeof *icn.m) };
}

void intuition_conn_down(void)
{
    icn.up = false;
}

void intuition_conn_serve(struct MsgPort *mp)
{
    struct IntuiMsg *m;

    while ((m = (struct IntuiMsg *)GetMsgSized(mp, sizeof *m))) {
        icn.got = *m;
        icn.answered = m->im_Msg.mn_Error == 0;
        icn.waiting = false;
    }
}

/* Ask intuition how things are unless an answer's awaited, connecting
 * first if need be; true once one has come (in icn.got). */
static bool asked(void)
{
    if (icn.answered) {
        icn.answered = false;
        return true;
    }
    conn_up();
    if (!icn.up || icn.waiting || !icn.m)
        return false;
    memset(icn.m, 0, sizeof *icn.m);
    icn.m->im_Msg.mn_Length = sizeof *icn.m;
    icn.m->im_Op = INTUI_INFO;
    icn.m->im_Task = FindTask(nullptr);
    if (PutMsg(server_conn(CONN_INTUITION), &icn.m->im_Msg) == 0)
        icn.waiting = true;
    return false;
}

/* --- The test. --- */

enum : uint8_t { W_START, W_UP, W_RUN, W_COUNTS, W_CUE, W_WITH, W_I_BACK, W_I_FREED, W_D_BACK,
                 W_END, W_STOP, W_FREED, W_DONE };

constexpr int64_t NO_INPUT = 1000;     /* window.c's: its windows ok, and nobody at the mouse */
constexpr uint64_t SURVIVE = 3, SURVIVE_MOUSE = 4;  /* window.c's second round, and with the mouse */

static struct {
    uint8_t step;
    uint64_t at;
    Handle client;
    uint32_t before;                /* display's buffers without intuition */
    uint32_t with;                  /* and with it */
    uint32_t handles;               /* intuition's, before the first round */
    uint64_t pages;
    unsigned starts, display_deaths, input_starts;
    bool input;                     /* window.c's input half ran */
} wt;

static void test_fail(const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: window test: FAILED: ");
    hw_put(&l, what);
    say(l.text);
    hwdb.errors++;
    if (wt.client) {
        KillProcess(wt.client);
        CloseHandle(wt.client);
        wt.client = 0;
    }
    intuition_want(false);
    wt.step = W_DONE;
}

static bool late(uint64_t ms)
{
    return hwdb.now - wt.at > ms * MS;
}

/* The client has ended: true, with its code (-1 if it faulted or was killed). */
static bool ended(int64_t *code)
{
    struct ProcessInfo pi;

    if (QueryProcess(wt.client, &pi) != 0 || pi.pi_State < PROCS_EXITED)
        return false;
    *code = pi.pi_State == PROCS_EXITED ? pi.pi_ExitCode : -1;
    CloseHandle(wt.client);
    wt.client = 0;
    return true;
}

static bool start(uint64_t arg)
{
    int64_t p = CreateProcess("window", arg, SIG_DEAD, 0);

    if (p < 0) {
        test_fail("window couldn't be started");
        return false;
    }
    wt.client = (Handle)p;
    wt.at = hwdb.now;
    return true;
}

static void next(uint8_t step)
{
    wt.step = step;
    wt.at = hwdb.now;
}

static void lines(void)
{
    say("V7.3 windows ok: a client's two windows composed on plane 1 from its own bitmaps, "
        "no copy over IPC, read back as drawn; refusals; frames paced by is_Safe");
    say("V7.3 windows ok: intuition killed, its screen buffers freed by display");
    if (wt.input)
        say("V7.4 input ok: focus by a click, keys at the active window alone, IDCMP events, "
            "a drag, the depth and close gadgets; a stopped client's window dragged, raised "
            "and taken off");
    else
        say("V7.4 input: not exercised: nobody moved the mouse, or intuition had no input");
    say("V7.5 supervision ok: kill-window: a killed client's two windows gone from the screen, its "
        "Areas unshared, intuition's handles and pages back where they were");
    say("V7.5 supervision ok: intuition killed under a client, restarted by hwdb, its buffers freed "
        "by display; the client saw the console, reconnected and opened its windows again");
    say("V7.5 supervision ok: display killed under intuition, which composed the screen again, the "
        "client's frames going on; input killed, its ring opened again, the pointer where it was");
}

bool window_test(void)
{
    struct ProcessInfo pi;
    struct unit *u;
    int64_t code;

    /* The second round ends only once it's seen every death. */
    if (wt.step >= W_CUE && wt.step < W_END && ended(&code)) {
        test_fail("window's second round ended early: its checks failed");
        return true;
    }
    switch (wt.step) {
    case W_START:
        if (!display_unit()) {
            say("V7.3 windows: not exercised: no screen here");
            say("V7.4 input: not exercised: no screen here");
            say("V7.5 supervision: not exercised: no screen here");
            return true;
        }
        /* display's own count first: what intuition takes must come back. */
        if (!wt.at)
            wt.at = hwdb.now;
        if (!display_ask()) {
            if (late(3000))
                test_fail("display didn't say how many buffers it holds");
            return wt.step == W_DONE;
        }
        wt.before = display_buffers();
        intuition_want(true);
        next(W_UP);
        return false;
    case W_UP:
        /* Up, with hwdb's connection: what it holds now, it must hold after. */
        if (!asked() || QueryProcess(ik.proc, &pi) != 0) {
            if (late(10000))
                test_fail("intuition didn't come up");
            return wt.step == W_DONE;
        }
        wt.handles = pi.pi_Handles;
        wt.pages = pi.pi_Pages;
        wt.starts = ik.starts;
        if (!start(0))
            return true;
        next(W_RUN);
        return false;
    case W_RUN:
        if (ik.starts != wt.starts || !ik.proc) {
            test_fail("intuition ended under its client");
            return true;
        }
        if (!ended(&code)) {
            if (late(90000))
                test_fail("window didn't finish");
            return wt.step == W_DONE;
        }
        if (code != 0 && code != NO_INPUT) {
            test_fail("window's checks failed");
            return true;
        }
        wt.input = code == 0;
        next(W_COUNTS);
        return false;
    case W_COUNTS:
        if (QueryProcess(ik.proc, &pi) != 0 || pi.pi_Handles != wt.handles || pi.pi_Pages != wt.pages) {
            if (late(3000)) {
                struct hw_line l = { 0 };

                hw_put(&l, "hwdb: window test: intuition holds ");
                hw_put_dec(&l, pi.pi_Handles);
                hw_put(&l, " handles and ");
                hw_put_dec(&l, pi.pi_Pages);
                hw_put(&l, " pages, not ");
                hw_put_dec(&l, wt.handles);
                hw_put(&l, " and ");
                hw_put_dec(&l, wt.pages);
                say(l.text);
                test_fail("intuition's handles and pages weren't back where they were");
            }
            return wt.step == W_DONE;
        }
        if (!start(wt.input ? SURVIVE | SURVIVE_MOUSE : SURVIVE))
            return true;
        next(W_CUE);
        return false;
    case W_CUE:
        /* Its windows checked, and the third opened: intuition killed. */
        if (!asked() || icn.got.im_Windows < 3) {
            if (late(20000))
                test_fail("window's second round never showed its three windows");
            return wt.step == W_DONE;
        }
        next(W_WITH);
        return false;
    case W_WITH:
        if (!display_ask()) {
            if (late(3000))
                test_fail("display didn't say how many buffers it holds");
            return wt.step == W_DONE;
        }
        wt.with = display_buffers();
        wt.starts = ik.starts;
        KillProcess(ik.proc);
        next(W_I_BACK);
        return false;
    case W_I_BACK:
        /* Started again, and the client's windows open on it. */
        if (ik.starts == wt.starts || !asked() || icn.got.im_Windows < 3) {
            if (late(20000))
                test_fail("intuition didn't come back with the client's windows");
            return wt.step == W_DONE;
        }
        next(W_I_FREED);
        return false;
    case W_I_FREED:
        if (!display_ask() || display_buffers() != wt.with) {
            if (late(3000))
                test_fail("display didn't free a dead intuition's buffers");
            return wt.step == W_DONE;
        }
        if (!(u = display_unit())) {
            test_fail("display wasn't running to be killed");
            return true;
        }
        wt.display_deaths = u->deaths;
        unit_kill(u);
        next(W_D_BACK);
        return false;
    case W_D_BACK:
        /* display back, and intuition's buffers taken from it again. */
        if (!(u = display_unit()) || u->deaths == wt.display_deaths || !display_ask() ||
            display_buffers() != wt.with - wt.before) {
            if (late(20000))
                test_fail("intuition didn't take its buffers again from the restarted display");
            return wt.step == W_DONE;
        }
        wt.before = 0;              /* the new display's, without intuition */
        wt.input_starts = input_kill();
        next(W_END);
        return false;
    case W_END:
        if (!ended(&code)) {
            if (late(60000))
                test_fail("window's second round didn't finish");
            return wt.step == W_DONE;
        }
        if (code != 0) {
            test_fail("window's checks across the servers' deaths failed");
            return true;
        }
        if (input_starts() == wt.input_starts) {
            test_fail("input didn't come back");
            return true;
        }
        intuition_want(false);
        next(W_STOP);
        return false;
    case W_STOP:
        if (ik.proc) {
            if (late(3000))
                test_fail("intuition wouldn't die");
            return wt.step == W_DONE;
        }
        next(W_FREED);
        return false;
    case W_FREED:
        if (!display_ask() || display_buffers() != wt.before) {
            if (late(3000))
                test_fail("display didn't free a dead intuition's buffers");
            return wt.step == W_DONE;
        }
        lines();
        wt.step = W_DONE;
        return true;
    default:
        return true;
    }
}
