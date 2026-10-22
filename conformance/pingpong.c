/*
 * conformance/pingpong.c -- DoMsg round trips with the echo server
 *
 * The number to compare with omega's 3,361 cycles on the X60 (docs §4.4).
 * Exits with the number of bad replies, or -1 if it can't connect. With
 * an argument, it exits with the cycles per round trip instead, or -1
 * on a bad reply (for the kernel's table, V3.5 costs).
 */
#include <planar/syscalls.h>

#include "cycles.h"
#include "echo.h"
#include "log.h"

constexpr unsigned ROUNDS = 20000;

int main(uint64_t table)
{
    struct MsgPort mp;
    struct EchoMsg *m;
    int64_t sig = AllocSignal(-1), err;
    struct CycleSum each = { 0 };
    uint64_t c0, bad = 0;

    while ((err = ConnectPort(&mp, "echo", sig, 0, 0)) == ERR_NOTFOUND)
        ;
    if (err < 0 || !(m = AllocMsg(&mp, sizeof *m)))
        return -1;
    m->em_Msg.mn_Length = sizeof *m;
    m->em_Value = 0;
    DoMsg(&mp, &m->em_Msg);         /* the server accepts, and settles */
    m->em_Value = 0;
    for (unsigned i = 0; i < ROUNDS; i++) {
        c0 = ReadCycles();
        bad += DoMsg(&mp, &m->em_Msg) != 0;
        CycleAdd(&each, c0, ReadCycles());
        bad += m->em_Value != i + 1;
    }
    KPrintF("pingpong: %lu DoMsg round trips, %lu cycles each (%lu moved CPU)", (uint64_t)ROUNDS,
            CycleMean(&each), each.dropped);
    if (table)
        return bad ? -1 : (int)CycleMean(&each);
    return (int)bad;
}
