/*
 * servers/hwdb/intuition.c -- the window test (v7-plan steps 3 and 4)
 *
 * killtest.c's T_WINDOW, on a machine with a screen: intuition started
 * as it's wanted (decision 4: on demand, not at every boot), and
 * conformance/window.c as its client, which checks the windows on the
 * screen and says what they cost, then works them with the mouse and
 * keys it asks for (on QEMU, the boot check sends them through QMP).
 * It ends 0, or NO_INPUT if nobody moved the mouse: a board with nobody
 * at it. Then intuition is killed, and display must free its buffers as
 * for any client that dies.
 * hwdb watches them with DISPLAY_INFO on its own connection (display.c).
 * Step 5 makes intuition a server hwdb keeps, killed under its clients.
 */
#include <planar/syscalls.h>

#include "server.h"

enum : uint8_t { W_START, W_RUN, W_STOP, W_FREED, W_DONE };

constexpr int64_t NO_INPUT = 1000;     /* window.c's: its windows ok, and nobody at the mouse */

static struct {
    uint8_t step;
    uint64_t at;
    Handle intuition, client;
    uint32_t before;                /* display's buffers before intuition */
    bool input;                     /* window.c's input half ran */
} wt;

static void test_fail(const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: window test: FAILED: ");
    hw_put(&l, what);
    say(l.text);
    hwdb.errors++;
    if (wt.client)
        KillProcess(wt.client);
    if (wt.intuition)
        KillProcess(wt.intuition);
    wt.step = W_DONE;
}

static bool late(uint64_t ms)
{
    return hwdb.now - wt.at > ms * MS;
}

/* 'h' has ended: true, with its code (-1 if it faulted or was killed). */
static bool ended(Handle h, int64_t *code)
{
    struct ProcessInfo pi;

    if (QueryProcess(h, &pi) != 0 || pi.pi_State < PROCS_EXITED)
        return false;
    *code = pi.pi_State == PROCS_EXITED ? pi.pi_ExitCode : -1;
    return true;
}

static Handle start(const char *name)
{
    int64_t p = CreateProcess(name, 0, SIG_DEAD, 0);

    return p < 0 ? 0 : (Handle)p;
}

bool window_test(void)
{
    int64_t code;

    switch (wt.step) {
    case W_START:
        if (!display_unit()) {
            say("V7.3 windows: not exercised: no screen here");
            say("V7.4 input: not exercised: no screen here");
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
        if (!(wt.intuition = start("intuition")) || !(wt.client = start("window"))) {
            test_fail("intuition or its client couldn't be started");
            return true;
        }
        wt.at = hwdb.now;
        wt.step = W_RUN;
        return false;
    case W_RUN:
        if (ended(wt.intuition, &code)) {
            test_fail("intuition ended under its client");
            return true;
        }
        if (!ended(wt.client, &code)) {
            if (late(60000))
                test_fail("window didn't finish");
            return wt.step == W_DONE;
        }
        CloseHandle(wt.client);
        wt.client = 0;
        if (code != 0 && code != NO_INPUT) {
            test_fail("window's checks failed");
            return true;
        }
        wt.input = code == 0;
        KillProcess(wt.intuition);
        wt.at = hwdb.now;
        wt.step = W_STOP;
        return false;
    case W_STOP:
        if (!ended(wt.intuition, &code)) {
            if (late(3000))
                test_fail("intuition wouldn't die");
            return wt.step == W_DONE;
        }
        CloseHandle(wt.intuition);
        wt.intuition = 0;
        wt.at = hwdb.now;
        wt.step = W_FREED;
        return false;
    case W_FREED:
        if (!display_ask() || display_buffers() != wt.before) {
            if (late(3000))
                test_fail("display didn't free a dead intuition's buffers");
            return wt.step == W_DONE;
        }
        say("V7.3 windows ok: a client's two windows composed on plane 1 from its own bitmaps, "
            "no copy over IPC, read back as drawn; refusals; frames paced by is_Safe");
        say("V7.3 windows ok: intuition killed, its screen buffers freed by display");
        if (wt.input)
            say("V7.4 input ok: focus by a click, keys at the active window alone, IDCMP events, "
                "a drag, the depth and close gadgets; a stopped client's window dragged, raised "
                "and taken off");
        else
            say("V7.4 input: not exercised: nobody moved the mouse, or intuition had no input");
        wt.step = W_DONE;
        return true;
    default:
        return true;
    }
}
