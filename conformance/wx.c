/*
 * conformance/wx.c -- write to its own code: W^X must stop it
 */
#include <planar/exec.h>

int main(void)
{
    volatile uint32_t *code = (volatile uint32_t *)(uintptr_t)main;

    KLog("writing to my own code");
    *code = 0;
    KLog("still running: code is writable");
    return 1;
}
