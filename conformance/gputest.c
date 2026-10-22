/*
 * conformance/gputest.c -- a client of a GPU's queue (v5-plan step 5)
 *
 * Started by hwdb's GPU test (servers/hwdb/gpu.c) with one argument:
 *
 * 0: one round. It lends the server a ring and a sync object, takes a
 *    queue, and runs jobs on it, each the smallest the GPU can be seen
 *    to do. On a CSF (the CIX): add 1 to the sync object (panthor's job
 *    epilogue, without the call), rung by its doorbell. On Rogue (the
 *    RV2): a NULL command and an UPDATE of the sync object to the next
 *    value (the DDK's kicksync), kicked by GPU_KICK. The first job's
 *    submit-to-update time and the kernel calls made meanwhile,
 *    machine-wide (QuerySystem), then JOBS more, spun on, then one
 *    waited for with WaitAddr, which the server wakes.
 * 1: jobs, without end: hwdb kills it with work in flight.
 * 2: a round, then it waits for the server to die (hwdb kills it),
 *    connects to the restarted one and does a second round.
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

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_GPU = 20;
constexpr uint64_t MS = 1000000;
constexpr uint64_t RING = 64 * 1024;
constexpr unsigned JOBS = 1000;
constexpr unsigned JOB = 8;                     /* instructions: one 64-byte line */

static unsigned step;

static int failed(const char *what)
{
    struct logbuf b = { 0 };

    log_str(&b, "gputest: check ");
    log_num(&b, step);
    log_str(&b, " failed: ");
    log_str(&b, what);
    KLog(b.text);
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
        if ((e = ConnectPort(&mp, "gpu.0", SIG_GPU, 4, 2 * sizeof *m)) != 0)
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
    struct logbuf b = { 0 };

    if (e == 0)
        return 0;
    log_str(&b, "gputest: ");
    log_str(&b, what);
    log_str(&b, ": error -");
    log_num(&b, (uint64_t)-e);
    KLog(b.text);
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
static void submit(void)
{
    uint64_t a = regs - 4, v = regs - 2;        /* the app's registers, pairs */
    uint64_t *j = &ring[qin->gq_Insert % RING / 8];

    if (kind == GPU_ROGUE) {
        submit_rogue();
        return;
    }

    j[0] = 1ull << 56 | a << 48 | (uint64_t)sync;               /* MOV48 ra, sync */
    j[1] = 1ull << 56 | v << 48 | 1;                            /* MOV48 rv, 1 */
    j[2] = 51ull << 56 | a << 40 | v << 32 | 1;                 /* SYNC_ADD64 [ra], rv */
    j[3] = 47ull << 56;                                         /* ERROR_BARRIER */
    for (unsigned i = 4; i < JOB; i++)
        j[i] = 0;                                               /* NOP */
    drv_io_barrier();
    qin->gq_Insert += JOB * 8;
    drv_io_barrier();
    *doorbell = 1;
    seqno++;
}

/* Spun on: true if it came by 'until'. */
static bool spin(uint64_t until)
{
    for (unsigned n = 0; sync->gs_Seqno != seqno; n++)
        if (n % 100000 == 0 && CurrentTime() > until)
            return false;
    return true;
}

static int round_(void)
{
    struct SystemInfo a, b;
    uint64_t t0, t1, total;
    struct logbuf lb = { 0 };

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
        log_str(&lb, "gputest: the first job didn't run: extract ");
        log_num(&lb, qout->gq_Extract);
        log_str(&lb, ", fault ");
        log_num(&lb, m->gm_Fault);
        KLog(lb.text);
        return (int)step;
    }
    QuerySystem(&b);
    t1 = CurrentTime();
    CHECK(sync->gs_Status == 0);
    log_str(&lb, "gputest: the first job ran in ");
    log_num(&lb, (t1 - t0) / 1000);
    log_str(&lb, " us, with ");
    log_num(&lb, b.si_Calls - a.si_Calls);
    log_str(&lb, " kernel calls made meanwhile, machine-wide");
    KLog(lb.text);

    /* JOBS more, each spun on. */
    t0 = CurrentTime();
    for (unsigned i = 0; i < JOBS; i++) {
        submit();
        CHECK(spin(CurrentTime() + 1000 * MS));
    }
    total = CurrentTime() - t0;
    /* The firmware writes how far it has read when it gets to it, not at
     * each job: it has moved on, and never past what was written. */
    for (uint64_t until = CurrentTime() + 100 * MS;
         qout->gq_Extract != qin->gq_Insert && CurrentTime() < until;)
        nap(1);
    CHECK(qout->gq_Extract > 0 && qout->gq_Extract <= qin->gq_Insert);
    if (qout->gq_Extract != qin->gq_Insert) {
        lb = (struct logbuf){ 0 };
        log_str(&lb, "gputest: the GPU says it has read ");
        log_num(&lb, qout->gq_Extract);
        log_str(&lb, " of ");
        log_num(&lb, qin->gq_Insert);
        log_str(&lb, " bytes written, every job done");
        KLog(lb.text);
    }

    /* One waited for: the server wakes the word. */
    t0 = CurrentTime();
    submit();
    while (sync->gs_Seqno != seqno && CurrentTime() < t0 + 1000 * MS)
        WaitAddr(&sync->gs_Seqno, seqno - 1, t0 + 1000 * MS);
    t1 = CurrentTime();
    CHECK(sync->gs_Seqno == seqno);
    CHECK(ask(GPU_STATUS) == 0 && m->gm_Faults == 0);

    lb = (struct logbuf){ 0 };
    log_str(&lb, "gputest: ");
    log_num(&lb, JOBS);
    log_str(&lb, " jobs in ");
    log_num(&lb, total / 1000);
    log_str(&lb, " us (");
    log_num(&lb, total / JOBS / 1000);
    log_str(&lb, " us each, submit to update); one waited for in ");
    log_num(&lb, (t1 - t0) / 1000);
    log_str(&lb, " us");
    KLog(lb.text);
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
