/*
 * conformance/pingpong.c -- DoMsg round trips with the echo server
 *
 * The number to compare with omega's 3,361 cycles on the X60 (docs §4.4).
 * Exits with the number of bad replies, or -1 if it can't connect.
 */
#include <planar/syscalls.h>

#include "cycles.h"
#include "echo.h"
#include "log.h"

constexpr unsigned ROUNDS = 20000;

int main(void)
{
    struct MsgPort mp;
    struct EchoMsg *m;
    struct logbuf b = { 0 };
    int64_t sig = AllocSignal(-1), err;
    uint64_t c0, cycles, bad = 0;

    while ((err = ConnectPort(&mp, "echo", sig, 0, 0)) == ERR_NOTFOUND)
        ;
    if (err < 0 || !(m = AllocMsg(&mp, sizeof *m)))
        return -1;
    m->em_Msg.mn_Length = sizeof *m;
    m->em_Value = 0;
    DoMsg(&mp, &m->em_Msg);         /* the server accepts, and settles */
    m->em_Value = 0;
    c0 = ReadCycles();
    for (unsigned i = 0; i < ROUNDS; i++)
        bad += DoMsg(&mp, &m->em_Msg) != 0 || m->em_Value != i + 1;
    cycles = ReadCycles() - c0;
    log_str(&b, "pingpong: ");
    log_num(&b, ROUNDS);
    log_str(&b, " DoMsg round trips, ");
    log_num(&b, cycles / ROUNDS);
    log_str(&b, " cycles each");
    KLog(b.text);
    return (int)bad;
}
