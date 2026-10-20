/*
 * conformance/wild.c -- a wild write: must fault, and take only this
 * process down
 */
#include <planar/exec.h>

int main(void)
{
    KLog("writing to 0x10");
    *(volatile uint32_t *)0x10 = 1;
    KLog("still running: the write didn't fault");
    return 1;
}
