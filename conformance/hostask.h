/*
 * conformance/hostask.h -- asking the host, on the host channel
 * (host-channel plan step 3)
 *
 * A test asks the host for input, or a screendump, with HostAsk: the
 * line "ask N VERB WORDS" to "host.0" (planar/devices/host.h), and the
 * host's "done N ok" back once it has done it (tools/boot-check.py, the
 * verbs its docstring gives). One ask at a time, numbered from the
 * clock's microseconds at the first, so one program's numbers don't run
 * into the last one's. If the channel's driver dies under the ask, it's
 * asked again on a new connection, as a class library recovers (§11.9),
 * with the same number, which the host answers without doing it twice.
 *
 * Where there's no channel (a board, the hosted build), HostAsk says
 * ERR_NOTFOUND at once: nobody's there to ask. Its connection's signal
 * is SIG_HOST, which a test leaves free.
 */
#ifndef CONFORMANCE_HOSTASK_H
#define CONFORMANCE_HOSTASK_H

#include <stdarg.h>

#include <planar/devices/host.h>
#include <planar/msg.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

constexpr uint64_t HOST_ASK_NS = 30000000000ULL;   /* an ask's answer: typing a long line takes seconds */
constexpr int64_t SIG_HOST = 29;

static struct {
    bool up, seen;                      /* connected; ever, so it's there to come back */
    struct MsgPort mp;
    struct HostMsg *m;
    uint64_t n;                         /* the last ask's number: from the clock, so no two programs' meet */
} host_;

static void host_close(void)
{
    if (host_.up)
        ClosePort(&host_.mp);
    host_.up = false;
}

/* Connected to the channel, waiting up to 'deadline' for it to come back
 * (0: not at all): 0, or ERR_NOTFOUND where there's none. */
static int64_t host_connect(uint64_t deadline)
{
    int64_t e;

    if (host_.up)
        return 0;
    if ((e = ConnectPortUntil(&host_.mp, "host.0", SIG_HOST, 2, 2 * sizeof *host_.m, host_.seen ? deadline : 0)) != 0)
        return e;
    if (!(host_.m = AllocMsg(&host_.mp, sizeof *host_.m))) {
        ClosePort(&host_.mp);
        return ERR_NOMEM;
    }
    host_.up = host_.seen = true;
    return 0;
}

/* One message of the channel's: 0, or why not (the connection closed if it died or was late). */
static int64_t host_do(uint32_t op, const char *text, uint32_t len, uint64_t deadline)
{
    int64_t e;

    memset(host_.m, 0, sizeof *host_.m);
    host_.m->hm_Msg.mn_Length = sizeof *host_.m;
    host_.m->hm_Op = op;
    host_.m->hm_Len = len;
    for (uint32_t i = 0; i < len; i++)
        host_.m->hm_Text[i] = text[i];
    if ((e = DoMsgUntil(&host_.mp, &host_.m->hm_Msg, deadline)) == 0)
        e = host_.m->hm_Msg.mn_Error;
    if (e == ERR_PORT_DIED || e == ERR_TIMEOUT)
        host_close();
    return e;
}

/* The host asked to do 'fmt': 0 once it's done; ERR_BADARG if the host
 * couldn't (its reason said); ERR_NOTFOUND if there's no channel. */
[[gnu::format(printf, 1, 2)]]
static inline int64_t HostAsk(const char *fmt, ...)
{
    char line[HOST_LINE], want[24];
    uint64_t until = CurrentTime() + HOST_ASK_NS;
    uint32_t len, wlen;
    va_list ap;
    int64_t e;

    if (!host_.n)
        host_.n = CurrentTime() / 1000;
    len = (uint32_t)SNPrintf(line, sizeof line, "ask %lu ", ++host_.n);
    va_start(ap, fmt);
    len += (uint32_t)VSNPrintf(line + len, sizeof line - len, fmt, ap);
    va_end(ap);
    if (len >= sizeof line)
        return ERR_BADARG;
    wlen = (uint32_t)SNPrintf(want, sizeof want, "done %lu ", host_.n);
    for (;;) {
        bool sent = false;

        /* Asked, and its answer read; again on a new connection if the driver died. */
        if ((e = host_connect(until)) == 0 && (e = host_do(HOST_WRITE, line, len, until)) == 0)
            sent = true;
        while (sent && (e = host_do(HOST_READ, nullptr, 0, until)) == 0) {
            const struct HostMsg *m = host_.m;
            bool mine = m->hm_Len >= wlen;

            for (uint32_t i = 0; mine && i < wlen; i++)
                mine = m->hm_Text[i] == want[i];
            if (!mine)
                continue;               /* an answer to an ask given up on */
            if (m->hm_Len == wlen + 2 && m->hm_Text[wlen] == 'o' && m->hm_Text[wlen + 1] == 'k')
                return 0;
            KLog("hostask: the host couldn't do what was asked");
            return ERR_BADARG;
        }
        if (e != ERR_PORT_DIED || CurrentTime() > until)
            return e;
    }
}

#endif /* CONFORMANCE_HOSTASK_H */
