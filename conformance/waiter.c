/*
 * conformance/waiter.c -- block in Wait for a signal nobody sends; the
 * kernel must be able to kill the process while it's blocked
 */
#include <planar/exec.h>

int main(void)
{
    KLog("waiting forever");
    Wait(1ULL << 20);
    KLog("Wait returned");
    return 1;
}
