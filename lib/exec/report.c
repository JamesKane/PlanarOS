/*
 * lib/exec/report.c -- ReportUnresponsive: a server that missed a
 * client's deadline, to hwdb (planar/msg.h, V6 step 7)
 *
 * One message on a connection of its own, made and closed here, so any
 * program can say so: a command of dos, the dos server of a block
 * driver. hwdb decides what to do (servers/hwdb/server.c, unresponsive).
 */
#include <planar/drv.h>
#include <planar/msg.h>

constexpr int64_t SIG_REPORT = 63;
constexpr uint64_t REPORT_NS = 1000000000;

int64_t ReportUnresponsive(const char *portName)
{
    struct MsgPort mp;
    struct HwdbMsg *m;
    int64_t e;
    unsigned i = 0;

    if ((e = ConnectPort(&mp, "hwdb", SIG_REPORT, 2, sizeof *m + 64)) != 0)
        return e;
    if (!(m = AllocMsg(&mp, sizeof *m))) {
        ClosePort(&mp);
        return ERR_NOMEM;
    }
    m->hm_Msg.mn_Length = sizeof *m;
    m->hm_Msg.mn_Error = 0;
    m->hm_Op = HWDB_UNRESPONSIVE;
    m->hm_Token = 0;
    for (; portName[i] && i < sizeof m->hm_PortName - 1; i++)
        m->hm_PortName[i] = portName[i];
    m->hm_PortName[i] = '\0';
    e = DoMsgUntil(&mp, &m->hm_Msg, CurrentTime() + REPORT_NS);
    if (e == 0)
        e = m->hm_Msg.mn_Error;
    ClosePort(&mp);
    return e;
}
