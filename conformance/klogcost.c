/*
 * conformance/klogcost.c -- what a KLog line costs its caller (review M12)
 *
 * The kernel holds the console from another CPU while this runs, as a
 * slow UART would. A line still costs a copy into the log's ring: the
 * call returns at once and the console shows the line later. Exits with
 * the call's time in microseconds.
 */
#include <planar/exec.h>

int main(void)
{
    uint64_t t = CurrentTime();

    KLog("klogcost: logged while the console was busy, shown after");
    t = CurrentTime() - t;
    return (int)(t / 1000 < 1000000 ? t / 1000 : 1000000);
}
