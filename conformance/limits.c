/*
 * conformance/limits.c -- one process can't take everything
 *
 * Creates tasks, allocates Areas and connects to "echo" until each stops
 * with ERR_LIMIT, which must come at the per-process limits rather than
 * when the whole system runs out (docs/10-code-review.md M11). Exits 0,
 * or with the number of the check that failed. Its tasks end with it.
 */
#include <planar/msg.h>
#include <planar/syscalls.h>

static void idle(uint64_t)
{
    Wait(1ULL << 30);
    ExitTask(0);
}

int main(void)
{
    static Handle areas[256];
    static struct MsgPort ports[64];
    unsigned tasks = 1, nareas = 0, nports = 0;
    int64_t r, sig = AllocSignal(-1);
    void *p;

    while ((r = CreateTask(idle, 0)) > 0)
        tasks++;
    if (r != ERR_LIMIT || tasks != 64)
        return 1;
    while ((r = AllocArea(4096, 0, &p)) > 0)
        areas[nareas++] = (Handle)r;
    if (r != ERR_LIMIT || nareas + tasks != 256)
        return 2;
    while (nareas)
        if (FreeArea(areas[--nareas]) != 0)
            return 3;
    while (nports < 64 && (r = ConnectPort(&ports[nports], "echo", sig, 2, 64)) == 0)
        nports++;
    if (r != ERR_LIMIT || nports != 32)
        return 4;
    return 0;
}
