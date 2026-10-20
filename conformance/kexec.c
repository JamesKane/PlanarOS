/*
 * conformance/kexec.c -- call kernel code from user mode
 *
 * The argument is a kernel function that only returns 42, reached
 * through the kernel's half of the address space. Calling it must fault
 * as an execute access: user mode never runs kernel memory, which on
 * AArch64 takes UXN on the kernel's mappings (docs/10-code-review.md H1).
 * Exits 42 if the call returned.
 */
#include <planar/exec.h>

int main(uint64_t addr)
{
    KLog("calling into the kernel");
    return (int)((uint64_t (*)(void))addr)();
}
