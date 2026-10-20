/*
 * conformance/logflood.c -- KLog as fast as it will go, with escapes
 *
 * 500 lines, each with terminal control sequences in it. The kernel
 * prints only what its rate limit allows and only printable bytes
 * (docs/10-code-review.md M12): boot-check fails a transcript with an
 * escape in it. Exits 0.
 */
#include <planar/exec.h>

int main(void)
{
    for (unsigned i = 0; i < 500; i++)
        KLog("flood \x1b[2J\x1b[H\a\b\r line");
    return 0;
}
