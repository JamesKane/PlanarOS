/*
 * conformance/vecfp.c -- a vector-FP instruction as the first FP use
 *
 * On RISC-V, a vector instruction with a scalar FP operand needs both
 * the vector and the FP state; the kernel must load whichever isn't
 * there yet rather than kill the task (docs/10-code-review.md H6).
 * Exits 42.
 */
#include "fpregs.h"

int main(void)
{
    return (int)fpregs_vecfp_first();
}
