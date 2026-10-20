/*
 * conformance/talker.c -- keeps a DoMsg conversation going with echo
 *
 * Until Ctrl-C, checking every reply, while the kernel adds competitors
 * and crashes around it (omega's client under load). Exits with the
 * number of bad replies, or -1 if it can't connect.
 */
#include <planar/syscalls.h>

#include "echo.h"
#include "log.h"

int main(void)
{
    int64_t sig = AllocSignal(-1), err;
    struct logbuf b = { 0 };
    struct EchoMsg *m;
    struct MsgPort mp;
    uint64_t i = 0, bad = 0;

    while ((err = ConnectPort(&mp, "echo", sig, 0, 0)) == ERR_NOTFOUND)
        ;
    if (err < 0 || !(m = AllocMsg(&mp, sizeof *m)))
        return -1;
    m->em_Msg.mn_Length = sizeof *m;
    m->em_Value = 0;
    while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C))
        if (DoMsg(&mp, &m->em_Msg) != 0 || m->em_Value != ++i)
            bad++;
    log_str(&b, "talker: ");
    log_num(&b, i);
    log_str(&b, " round trips under load, ");
    log_num(&b, bad);
    log_str(&b, " bad");
    KLog(b.text);
    return (int)bad;
}
