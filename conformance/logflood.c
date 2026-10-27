/*
 * conformance/logflood.c -- KLog as fast as it will go, with escapes
 *
 * 500 lines, each with terminal control sequences in it. The kernel
 * prints only what its rate limit allows and only printable bytes
 * (docs/10-code-review.md M12): boot-check fails a transcript with an
 * escape in it. The first 32 lines are taken (the burst); then KLog says
 * ERR_FULL for each it drops, and the flood must see at least 400 of
 * them. Exits 0, or 1 if KLog didn't say what it dropped.
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

int main(void)
{
    unsigned full = 0;

    for (unsigned i = 0; i < 500; i++)
        full += KLog("flood \x1b[2J\x1b[H\a\b\r line") == ERR_FULL;
    return full >= 400 ? 0 : 1;
}
