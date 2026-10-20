/*
 * conformance/overrun.c -- run off the bottom of the stack: must fault in
 * the guard page below it, and take only this process down
 */
#include <planar/exec.h>

#pragma clang diagnostic ignored "-Winfinite-recursion"     /* the point */

static int deeper(int n)
{
    volatile char frame[512];

    frame[0] = (char)n;
    frame[511] = (char)n;
    return deeper(n + 1) + frame[0];
}

int main(void)
{
    KLog("recursing without end");
    return deeper(0);
}
