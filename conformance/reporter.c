/*
 * conformance/reporter.c -- a client saying a server missed a deadline
 * (V6 step 7)
 *
 * hwdb's kill test starts it with a port's name, up to 8 bytes, as its
 * argument; it calls ReportUnresponsive with it, and hwdb must restart
 * the unit serving that port (servers/hwdb/killtest.c, report_step).
 * Exits 0 if hwdb took the report.
 */
#include <planar/msg.h>

int main(uint64_t arg)
{
    char name[9];

    for (unsigned i = 0; i < 8; i++)
        name[i] = (char)(arg >> 8 * i);
    name[8] = '\0';
    return ReportUnresponsive(name) == 0 ? 0 : 1;
}
