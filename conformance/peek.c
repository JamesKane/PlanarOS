/*
 * conformance/peek.c -- read the kernel address the kernel passed in:
 * must fault (user mode can't see the kernel half)
 */
#include <planar/exec.h>

int main(uint64_t kernel_addr)
{
    KLog("reading a kernel address");
    (void)*(volatile uint64_t *)(uintptr_t)kernel_addr;
    KLog("still running: the kernel is readable");
    return 1;
}
