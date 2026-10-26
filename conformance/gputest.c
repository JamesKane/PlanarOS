/*
 * conformance/gputest.c -- a client of a GPU's queue (v5-plan step 5)
 *
 * Started by hwdb's GPU tests (servers/hwdb/gpu.c) with one argument:
 *
 * 0: one round. It lends the server a ring and a sync object, takes a
 *    queue, and runs jobs on it, each the smallest the GPU can be seen
 *    to do. On a CSF (the CIX): add 1 to the sync object (panthor's job
 *    epilogue, without the call), rung by its doorbell. On Rogue (the
 *    RV2): a NULL command and an UPDATE of the sync object to the next
 *    value (the DDK's kicksync), kicked by GPU_KICK. The first job's
 *    submit-to-update time and the kernel calls made meanwhile,
 *    machine-wide (QuerySystem), then JOBS more, spun on, with the
 *    calls, switches and crossings each cost (step 7), then WAITED each
 *    waited for with WaitAddr, which the server wakes once told by
 *    GPU_WAIT. Then, on a CSF, a job that faults, heard of as GPU_EVENT's
 *    answer (V6 step 8).
 * 1: jobs, without end: hwdb kills it with work in flight.
 * 2: a round, then it waits for the server to die (hwdb kills it),
 *    connects to the restarted one and does a second round.
 * 3 | seconds << 8: jobs without a pause for that long, while hwdb kills
 *    other servers and another client around it (step 7's load test).
 *    No job may take a second unless the GPU server has died, and then
 *    it connects to the restarted one and goes on.
 *
 * It ends 0 if every round's jobs ran, 3 if there's no GPU up to run
 * them (its firmware never came), and otherwise with a failed check's
 * number.
 */
#include <stddef.h>

#include <planar/devices/gpu.h>
#include <planar/drv.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr int64_t SIG_GPU = 20;
constexpr uint64_t MS = 1000000;
constexpr uint64_t RING = 64 * 1024;
constexpr unsigned JOBS = 1000, WAITED = 100;
constexpr unsigned JOB = 8;                     /* instructions: one 64-byte line */

static unsigned step;

static int failed(const char *what)
{
    KPrintF("gputest: check %lu failed: %s", (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static struct MsgPort mp;
static struct GpuMsg *m;
static uint64_t *ring;
static struct GpuSync *sync;
static struct GpuQueueIn *qin;
static const struct GpuQueueOut *qout;
static volatile uint32_t *doorbell;
static unsigned regs;
static uint32_t kind;
static uint64_t seqno;
static uint32_t sync_device;            /* where the GPU sees the sync object */

static void nap(uint64_t ms)
{
    static uint64_t never;

    WaitAddr(&never, 0, CurrentTime() + ms * MS);
}

static int64_t ask(uint32_t op)
{
    m->gm_Msg.mn_Length = sizeof *m;
    m->gm_Op = op;
    if (DoMsg(&mp, &m->gm_Msg) != 0)
        return ERR_PORT_DIED;
    return m->gm_Msg.mn_Error;
}

/* Connected and open, once the server has its firmware up. */
static int64_t take(void)
{
    int64_t e = ERR_NOTFOUND;

    for (uint64_t until = CurrentTime() + 30000 * MS; CurrentTime() < until; nap(50)) {
        /* Its port, as soon as it's made; then the server may still be
         * bringing its firmware up (GPU_OPEN refused until it has). */
        if ((e = ConnectPortUntil(&mp, "gpu.0", SIG_GPU, 4, 2 * sizeof *m, until)) != 0)
            continue;
        if (!(m = AllocMsg(&mp, sizeof *m)))
            return ERR_NOMEM;
        memset(m, 0, sizeof *m);
        m->gm_Task = FindTask(nullptr);
        if ((e = ask(GPU_OPEN)) == 0) {
            regs = m->gm_Regs;
            kind = m->gm_Kind;
            return 0;
        }
        ClosePort(&mp);
        if (e == ERR_NOSYS)
            return e;                   /* the server gave the GPU up */
    }
    return e;
}

static int64_t lend(void *at, uint64_t size)
{
    struct AreaInfo ai;

    if (QueryArea(at, &ai) != 0 || ShareArea(ai.ai_Area, m->gm_Server, AREAR_READ | AREAR_WRITE))
        return ERR_RIGHTS;
    m->gm_Address = (uint64_t)at;
    m->gm_Size = size;
    return ask(GPU_MAP);
}

/* An error, said: the server's answer, for the log. */
static int64_t said(int64_t e, const char *what)
{
    if (e == 0)
        return 0;
    KPrintF("gputest: %s: error -%lu", what, (uint64_t)-e);
    return e;
}

/* The ring and the sync object, lent and mapped, and the queue. */
static int queue(void)
{
    void *at, *area;

    /* The ring aligned to its size, as the server asks: an Area is only
     * page aligned, so twice the size, and the ring in it. */
    CHECK(AllocArea(2 * RING, AREAF_NC, &at) >= 0);
    area = at;
    ring = (uint64_t *)(((uint64_t)at + RING - 1) & ~(RING - 1));
    CHECK(AllocArea(4096, AREAF_NC, &at) >= 0);
    sync = at;
    seqno = 0;
    CHECK(said(lend(area, 2 * RING), "lending the ring") == 0);
    CHECK(said(lend(sync, 4096), "lending the sync object") == 0);
    sync_device = (uint32_t)m->gm_Device;
    m->gm_Ring = (uint64_t)ring;
    m->gm_RingSize = RING;
    m->gm_Sync = (uint64_t)sync;
    CHECK(said(ask(GPU_QUEUE), "GPU_QUEUE") == 0);
    qin = (struct GpuQueueIn *)m->gm_Input;
    qout = (const struct GpuQueueOut *)m->gm_Output;
    doorbell = (volatile uint32_t *)m->gm_Doorbell;
    CHECK(kind == GPU_CSF ? regs >= 8 && doorbell : kind == GPU_ROGUE && !doorbell);
    return 0;
}

/* A Rogue client CCB command's header (rogue_fwif_ccb_cmd_header): its
 * type and its payload's size; the rest, job references and work
 * estimates, zero. */
static void rogue_header(uint8_t *at, uint32_t type, uint32_t size)
{
    uint32_t *w = (uint32_t *)at;

    w[0] = type;
    w[1] = size;
    for (unsigned i = 2; i < 10; i++)
        w[i] = 0;
}

/* One job on Rogue: a NULL command, then an UPDATE of the sync object's
 * low word to the next seqno. A command doesn't wrap: where the ring's
 * end is too close, a PADDING command to it first. */
static void submit_rogue(void)
{
    constexpr uint32_t NULL_CMD = 0x2abc80d2, UPDATE = 0x2abc00d5, PADDING = 0x2abc00dd;
    constexpr uint32_t HEADER = 40, JOB_BYTES = 2 * HEADER + 8;
    uint32_t at = (uint32_t)(qin->gq_Insert % RING);
    uint8_t *r = (uint8_t *)ring;

    if (RING - at < JOB_BYTES) {
        rogue_header(r + at, PADDING, RING - at - HEADER);
        qin->gq_Insert += RING - at;
        at = 0;
    }
    rogue_header(r + at, NULL_CMD, 0);
    rogue_header(r + at + HEADER, UPDATE, 8);
    ((uint32_t *)(r + at + 2 * HEADER))[0] = sync_device;            /* the UFO: where */
    ((uint32_t *)(r + at + 2 * HEADER))[1] = (uint32_t)(seqno + 1);  /* and what */
    drv_io_barrier();
    qin->gq_Insert += JOB_BYTES;
    drv_io_barrier();
    seqno++;
    ask(GPU_KICK);
}

/* One job written and rung: add 1 to the sync object. */
/* A job adding 1 at 'at': the sync object, or for the fault, nothing. */
static void submit_at(uint64_t at)
{
    uint64_t a = regs - 4, v = regs - 2;        /* the app's registers, pairs */
    uint64_t *j = &ring[qin->gq_Insert % RING / 8];

    j[0] = 1ull << 56 | a << 48 | at;                           /* MOV48 ra, at */
    j[1] = 1ull << 56 | v << 48 | 1;                            /* MOV48 rv, 1 */
    j[2] = 51ull << 56 | a << 40 | v << 32 | 1;                 /* SYNC_ADD64 [ra], rv */
    j[3] = 47ull << 56;                                         /* ERROR_BARRIER */
    for (unsigned i = 4; i < JOB; i++)
        j[i] = 0;                                               /* NOP */
    drv_io_barrier();
    qin->gq_Insert += JOB * 8;
    drv_io_barrier();
    *doorbell = 1;
}

static void submit(void)
{
    if (kind == GPU_ROGUE) {
        submit_rogue();             /* moves seqno on itself */
    } else {
        submit_at((uint64_t)sync);
        seqno++;
    }
}

/* Spun on: true if it came by 'until'. */
static bool spin(uint64_t until)
{
    /* Not at the first look: a job's calls are counted (round_). */
    for (unsigned n = 1; sync->gs_Seqno != seqno; n++)
        if (n % 100000 == 0 && CurrentTime() > until)
            return false;
    return true;
}

/* n / d in tenths, to be said with one decimal: t / 10, ".", t % 10. */
static uint64_t tenths(uint64_t n, uint64_t d)
{
    return (n * 10 + d / 2) / d;
}

static int round_(void)
{
    struct SystemInfo a, b;
    uint64_t t0, t1, total, until, waited = 0, worst = 0;
    uint64_t t4[4];

    if (queue() != 0)
        return (int)step;

    /* The first: its time, and the calls made meanwhile (QuerySystem's
     * own and CurrentTime's are counted too). */
    t0 = CurrentTime();
    QuerySystem(&a);
    submit();
    step++;
    if (!spin(t0 + 1000 * MS)) {
        ask(GPU_STATUS);
        KPrintF("gputest: the first job didn't run: extract %lu, fault %lu",
                (uint64_t)qout->gq_Extract, (uint64_t)m->gm_Fault);
        return (int)step;
    }
    QuerySystem(&b);
    t1 = CurrentTime();
    CHECK(sync->gs_Status == 0);
    KPrintF("gputest: the first job ran in %lu us, with %lu kernel calls made meanwhile, "
            "machine-wide",
            (uint64_t)((t1 - t0) / 1000), (uint64_t)(b.si_Calls - a.si_Calls));

    /* JOBS more, each spun on, with what they cost machine-wide: one
     * deadline for them all, so the loop makes no calls of its own. */
    t0 = CurrentTime();
    until = t0 + 10000 * MS;
    QuerySystem(&a);
    for (unsigned i = 0; i < JOBS; i++) {
        submit();
        CHECK(spin(until));
    }
    QuerySystem(&b);
    total = CurrentTime() - t0;
    t4[0] = tenths(b.si_Calls - a.si_Calls, JOBS);
    t4[1] = tenths(b.si_Switches - a.si_Switches, JOBS);
    t4[2] = tenths(b.si_Crossings - a.si_Crossings, JOBS);
    t4[3] = tenths(b.si_Interrupts - a.si_Interrupts, JOBS);
    KPrintF("gputest: each job cost, machine-wide, %lu.%lu kernel calls, %lu.%lu task switches, "
            "%lu.%lu crossings and %lu.%lu interrupts",
            t4[0] / 10, t4[0] % 10, t4[1] / 10, t4[1] % 10,
            t4[2] / 10, t4[2] % 10, t4[3] / 10, t4[3] % 10);
    /* Spun on, not told of by GPU_WAIT: the CSF's server takes no
     * interrupt for them (V6 step 8; it took one a job before). Rogue's
     * firmware interrupts at every update. */
    CHECK(kind != GPU_CSF || t4[3] < 5);
    /* The firmware writes how far it has read when it gets to it, not at
     * each job: it has moved on, and never past what was written. */
    for (uint64_t until = CurrentTime() + 100 * MS;
         qout->gq_Extract != qin->gq_Insert && CurrentTime() < until;)
        nap(1);
    CHECK(qout->gq_Extract > 0 && qout->gq_Extract <= qin->gq_Insert);
    if (qout->gq_Extract != qin->gq_Insert) {
        KPrintF("gputest: the GPU says it has read %lu of %lu bytes written, every job done",
                (uint64_t)qout->gq_Extract, (uint64_t)qin->gq_Insert);
    }

    /* WAITED waited for: told it will be (GPU_WAIT), the server wakes the
     * word; spun on, above, it wasn't told and took no interrupt. */
    for (unsigned i = 0; i < WAITED; i++) {
        t0 = CurrentTime();
        submit();
        m->gm_Seqno = seqno;
        CHECK(ask(GPU_WAIT) == 0);
        while (sync->gs_Seqno != seqno && CurrentTime() < t0 + 1000 * MS)
            WaitAddr(&sync->gs_Seqno, seqno - 1, t0 + 1000 * MS);
        t1 = CurrentTime();
        CHECK(sync->gs_Seqno == seqno);
        waited += t1 - t0;
        if (t1 - t0 > worst)
            worst = t1 - t0;
    }
    CHECK(ask(GPU_STATUS) == 0 && m->gm_Faults == 0);

    KPrintF("gputest: %lu jobs in %lu us (%lu us each, submit to update); %lu waited for, "
            "%lu us each (worst %lu us)",
            (uint64_t)JOBS, (uint64_t)(total / 1000), (uint64_t)(total / JOBS / 1000),
            (uint64_t)WAITED, (uint64_t)(waited / WAITED / 1000), (uint64_t)(worst / 1000));
    return 0;
}

/* On a CSF, a job that writes where nothing is mapped: the fault must
 * come as GPU_EVENT's answer, held by the server until it does, or at
 * once if it came first (V6 step 8). The server stops the queue after. */
static int fault(void)
{
    constexpr uint64_t NOWHERE = 0x10000;   /* in the client's GPU space, unmapped */
    struct GpuMsg *ev;
    uint64_t t0;

    if (kind != GPU_CSF)
        return 0;
    CHECK((ev = AllocMsg(&mp, sizeof *ev)));
    memset(ev, 0, sizeof *ev);
    ev->gm_Msg.mn_Length = sizeof *ev;
    ev->gm_Op = GPU_EVENT;
    t0 = CurrentTime();
    submit_at(NOWHERE);
    CHECK(DoMsgUntil(&mp, &ev->gm_Msg, t0 + 2000 * MS) == 0);
    CHECK(ev->gm_Msg.mn_Error == 0 && ev->gm_Fault != 0);
    CHECK(ev->gm_FaultAddress >> 12 == NOWHERE >> 12 && ev->gm_Faults == 1);
    KPrintF("gputest: a job's fault at %#lx came as a message in %lu us (status %#lx)",
            ev->gm_FaultAddress, (uint64_t)((CurrentTime() - t0) / 1000),
            (uint64_t)ev->gm_Fault);
    return 0;
}

/* Mode 3: jobs for 'seconds' under hwdb's kills. */
static int load(uint64_t seconds)
{
    uint64_t start = CurrentTime(), end = start + seconds * 1000 * MS, last, now, gap = 0;
    uint64_t jobs = 0, since = 0;
    unsigned deaths = 0;
    int r;

    if ((r = queue()) != 0)
        return r;
    KLog("gputest: running jobs under load");
    last = CurrentTime();
    while ((now = CurrentTime()) < end) {
        /* A dead server's doorbell faults: none rung once it's gone. A
         * death between this look and the ring kills us too, but that's
         * a few instructions in a job's 10 us and more. */
        if (!PortGone(&mp)) {
            submit();
            if (spin(now + 1000 * MS)) {
                now = CurrentTime();
                if (now - last > gap)
                    gap = now - last;
                last = now;
                jobs++;
                since++;
                continue;
            }
        }
        /* A second and no update: only the server's death may do that. */
        CHECK(PortGone(&mp));
        ClosePort(&mp);
        CHECK(take() == 0);
        if ((r = queue()) != 0)
            return r;
        deaths++;
        since = 0;
        last = CurrentTime();
    }
    CHECK(since > 0);                   /* jobs ran after the last death */
    KPrintF("gputest: %lu jobs in %lu s under load, the longest between two %lu us, but for %lu %s",
            (uint64_t)jobs, (uint64_t)seconds, (uint64_t)(gap / 1000), (uint64_t)deaths,
            deaths == 1 ? "death of the GPU server" : "deaths of the GPU server");
    return 0;
}

int main(uint64_t mode)
{
    int64_t e;
    int r;

    step++;
    if ((e = take()) != 0) {
        KLog("gputest: no GPU up to run jobs");
        return 3;
    }
    if ((mode & 0xff) == 3)
        return load(mode >> 8);
    if (mode == 1) {
        if ((r = queue()) != 0)
            return r;
        KLog("gputest: running jobs");
        for (;;) {
            submit();
            spin(CurrentTime() + 1000 * MS);
        }
    }
    if ((r = round_()) != 0)
        return r;
    if (mode == 0 && (r = fault()) != 0)
        return r;
    if (mode == 2) {
        /* The server dies under us: hwdb kills it. */
        for (uint64_t until = CurrentTime() + 30000 * MS; !PortGone(&mp) && CurrentTime() < until;)
            nap(20);
        CHECK(PortGone(&mp));
        ClosePort(&mp);
        CHECK(take() == 0);
        if ((r = round_()) != 0)
            return r;
    }
    KLog("gputest: ok");
    return 0;
}
