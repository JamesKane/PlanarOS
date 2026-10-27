/*
 * conformance/inputtest.c -- a client of input's ring (v5-plan step 4)
 *
 * Started by hwdb's input test (servers/hwdb/input.c), as the Shell takes
 * the ring, with one argument:
 *
 * 0, 1: one round. It takes the ring and asks for keys ("inputtest: send
 *    keys ..."), which on QEMU the boot check types through QMP
 *    (tools/boot-check.py), and checks they came as typed: "Hi 5" and
 *    Enter, each key down then up, Shift held over the H. Then it asks
 *    for the mouse to move (10, -5) and its left button to be clicked,
 *    and checks that came too.
 * 2: a round, then it waits for input to die under it (hwdb kills it),
 *    connects to the restarted input, takes its new ring, and does a
 *    second round.
 *
 * It ends with 0 if every round's keys came and were right, 2 if none
 * came at all (nobody typed: a board with nobody at it), and otherwise
 * with a failed check's number.
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/syscalls.h>

#include "hostask.h"
#include "log.h"

constexpr int64_t SIG_INPUT = 20;
constexpr uint64_t MS = 1000000;
constexpr uint64_t WAIT = 5000 * MS;        /* for someone to type */

static unsigned step;

static int failed(const char *what)
{
    KPrintF("inputtest: check %lu failed: %s", (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static struct MsgPort mp;
static struct InputMsg *m;
static const struct InputRing *ring;
static uint64_t tail;
static uint32_t mice;

static void nap(uint64_t ms)
{
    static uint64_t never;              /* a word nobody wakes */

    WaitAddr(&never, 0, CurrentTime() + ms * MS);
}

static int64_t ask(uint32_t op)
{
    m->im_Msg.mn_Length = sizeof *m;
    m->im_Op = op;
    m->im_Task = FindTask(nullptr);
    if (DoMsg(&mp, &m->im_Msg) != 0)
        return ERR_PORT_DIED;
    return m->im_Msg.mn_Error;
}

/* Connected, and the ring ours, with a keyboard to type on: input may
 * be restarting, and the last client may not have let it go yet. */
static int64_t take(void)
{
    int64_t e = ERR_NOTFOUND;

    for (uint64_t until = CurrentTime() + 10000 * MS; CurrentTime() < until; nap(20)) {
        if ((e = ConnectPortUntil(&mp, "input", SIG_INPUT, 4, 2 * sizeof *m, until)) != 0)
            continue;
        if (!(m = AllocMsg(&mp, sizeof *m)))
            return ERR_NOMEM;
        memset(m, 0, sizeof *m);
        /* Its keyboard there: after input's death, the drivers come back
         * to it in their own time. */
        while ((e = ask(INPUT_INFO)) == 0 && (!m->im_Keyboards || m->im_Mice < mice) &&
               CurrentTime() < until)
            nap(20);
        if (e == 0 && m->im_Keyboards)
            mice = m->im_Mice;
        if (e == 0 && (e = ask(INPUT_OPEN)) == 0) {
            ring = (const struct InputRing *)m->im_Ring;
            tail = ring->ir_Head;
            return 0;
        }
        ClosePort(&mp);
    }
    return e;
}

/* The next event, by 'until'; false if none came. */
static bool next(struct InputEvent *e, uint64_t until)
{
    for (;;) {
        uint64_t head = __atomic_load_n(&ring->ir_Head, __ATOMIC_ACQUIRE);

        if (head - tail > INPUT_RING)
            tail = head - INPUT_RING;           /* lost: the oldest overwritten */
        if (tail != head) {
            *e = ring->ir_Event[tail % INPUT_RING];
            if (__atomic_load_n(&ring->ir_Head, __ATOMIC_ACQUIRE) - tail > INPUT_RING)
                continue;                       /* overwritten as it was read */
            tail++;
            return true;
        }
        if (CurrentTime() >= until || PortGone(&mp))
            return false;
        WaitAddr(&ring->ir_Head, head, until);
    }
}

/* Asked of the host, where there's one; on a board, someone types. */
static void send(const char *what)
{
    HostAsk("%s", what);
}

/* One round: 1 if it was typed and right, 0 if nothing came, or the
 * negated number of the check that failed. */
static int round_(void)
{
    static const char want[] = "Hi 5\n";
    struct InputEvent e;
    char got[16];
    unsigned ngot = 0, downs = 0, ups = 0, presses = 0, releases = 0;
    int32_t dx = 0, dy = 0;
    bool shifted = false;
    uint64_t until;

    send("keys shift-h i spc 5 ret");
    until = CurrentTime() + WAIT;
    while ((ngot < sizeof want - 1 || downs != ups) && next(&e, until)) {
        if (e.ie_Class != IECLASS_KEY)
            continue;
        until = CurrentTime() + WAIT;
        if (e.ie_Flags & IEF_REPEAT)
            return -failed("a key repeated: it was held too long");
        if (e.ie_Flags & IEF_UP) {
            ups++;
            continue;
        }
        downs++;
        if (e.ie_Char && ngot < sizeof got)
            got[ngot++] = (char)e.ie_Char;
        if (e.ie_Char == 'H')
            shifted = (e.ie_Qualifier & IEQ_SHIFT) != 0;
    }
    if (downs == 0) {
        step++;
        return PortGone(&mp) ? -failed("input went in the round") : 0;
    }
    step++;
    if (ngot != sizeof want - 1)
        return -failed("not all the keys came");
    for (unsigned i = 0; i < ngot; i++) {
        step++;
        if (got[i] != want[i])
            return -failed("a key typed the wrong character");
    }
    step++;
    if (!shifted)
        return -failed("Shift wasn't held over the H");
    step++;
    if (downs != ups || downs != 6)
        return -failed("each key didn't go down and up once");
    if (!mice)
        return 1;

    send("mouse 10 -5 left");
    until = CurrentTime() + WAIT;
    while ((dx != 10 || dy != -5 || releases == 0) && next(&e, until)) {
        if (e.ie_Class == IECLASS_MOVE) {
            dx += e.ie_X;
            dy += e.ie_Y;
        } else if (e.ie_Class == IECLASS_BUTTON && e.ie_Code == 0) {
            if (e.ie_Flags & IEF_UP)
                releases++;
            else
                presses++;
        }
    }
    step++;
    if (dx != 10 || dy != -5)
        return -failed("the mouse didn't move (10, -5)");
    step++;
    if (presses != 1 || releases != 1)
        return -failed("the left button wasn't clicked once");
    return 1;
}

int main(uint64_t mode)
{
    int typed = 0, r;

    CHECK(take() == 0);
    CHECK((r = round_()) >= 0);
    typed += r;
    if (mode == 2) {
        /* input dies under us: hwdb kills it. */
        for (uint64_t until = CurrentTime() + 30000 * MS; !PortGone(&mp) && CurrentTime() < until;)
            WaitAddr(&ring->ir_Head, ring->ir_Head, CurrentTime() + 100 * MS);
        CHECK(PortGone(&mp));
        ClosePort(&mp);
        CHECK(take() == 0);
        CHECK((r = round_()) >= 0);
        typed += r;
    }
    if (typed == 0) {
        KLog("inputtest: nobody typed");
        return 2;
    }
    CHECK(typed == (mode == 2 ? 2 : 1));
    KLog("inputtest: ok");
    return 0;
}
