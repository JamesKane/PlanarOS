/*
 * conformance/client.c -- talks to the echo server
 *
 * With argument 0: asynchronous round trips (PutMsg, WaitPort, GetMsg),
 * then synchronous ones (DoMsg), checking every reply. With argument n:
 * streams n messages, keeping the ring full, so the kernel can count how
 * few calls each side makes. Exits with the number of bad replies, or
 * -1 if it can't connect. From omega's conformance/client.c.
 */
#include <planar/syscalls.h>

#include "cycles.h"
#include "echo.h"
#include "log.h"

constexpr unsigned ROUNDS = 2000;
constexpr uint32_t SLOTS = 64;

static void report(const char *what, uint64_t count, uint64_t cycles, uint64_t bad)
{
    struct logbuf b = { 0 };

    log_str(&b, what);
    log_str(&b, ": ");
    log_num(&b, count);
    log_str(&b, ", ");
    log_num(&b, count ? cycles / count : 0);
    log_str(&b, " cycles each, ");
    log_num(&b, bad);
    log_str(&b, " bad");
    KLog(b.text);
}

static uint64_t round_trips(struct MsgPort *mp)
{
    struct EchoMsg *m = AllocMsg(mp, sizeof *m);
    uint64_t c0, bad = 0;

    m->em_Msg.mn_Length = sizeof *m;
    m->em_Value = 0;
    c0 = ReadCycles();
    for (unsigned i = 0; i < ROUNDS; i++) {
        PutMsg(mp, &m->em_Msg);
        bad += WaitPort(mp) != &m->em_Msg || m->em_Value != i + 1;
    }
    report("client: PutMsg/WaitPort round trips", ROUNDS, ReadCycles() - c0, bad);

    m->em_Value = 0;
    c0 = ReadCycles();
    for (unsigned i = 0; i < ROUNDS; i++)
        bad += DoMsg(mp, &m->em_Msg) != 0 || m->em_Value != i + 1;
    report("client: DoMsg round trips", ROUNDS, ReadCycles() - c0, bad);
    return bad;
}

static uint64_t stream(struct MsgPort *mp, uint64_t total)
{
    struct EchoMsg *m[SLOTS];
    uint64_t want[SLOTS], c0, sent = 0, done = 0, bad = 0;

    for (unsigned i = 0; i < SLOTS; i++) {
        m[i] = AllocMsg(mp, sizeof *m[i]);
        m[i]->em_Msg.mn_Length = sizeof *m[i];
        m[i]->em_Seq = i;
        m[i]->em_Value = (uint64_t)i << 32;
        want[i] = m[i]->em_Value + 1;
    }
    c0 = ReadCycles();
    for (unsigned i = 0; i < SLOTS && sent < total; i++, sent++)
        PutMsg(mp, &m[i]->em_Msg);
    while (done < total) {
        struct EchoMsg *r = (struct EchoMsg *)WaitPort(mp);
        uint64_t seq;

        if (!r)
            return bad + total - done;
        seq = r->em_Seq;
        if (seq >= SLOTS || r != m[seq] || r->em_Value != want[seq]) {
            bad++;
            seq %= SLOTS;
            r = m[seq];
        }
        done++;
        want[seq] = r->em_Value + 1;
        if (sent < total && PutMsg(mp, &r->em_Msg) == 0)
            sent++;
    }
    report("client: streamed", total, ReadCycles() - c0, bad);
    return bad;
}

int main(uint64_t n)
{
    struct MsgPort mp;
    int64_t sig = AllocSignal(-1), err;

    while ((err = ConnectPort(&mp, "echo", sig, SLOTS, 8192)) == ERR_NOTFOUND)
        ;       /* the server isn't up yet */
    if (err < 0) {
        KLog("client: can't connect to echo");
        return -1;
    }
    return (int)(n ? stream(&mp, n) : round_trips(&mp));
}
