/*
 * conformance/hello.c -- the smallest program: log a line and exit
 */
#include <planar/exec.h>

int main(void)
{
    KLog("hello from user mode");
    return 7;
}
