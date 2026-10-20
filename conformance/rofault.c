/*
 * conformance/rofault.c -- write to an Area made read-only: must fault
 */
#include <planar/exec.h>

int main(void)
{
    uint8_t *p;
    Handle a = AllocArea(4096, 0, (void **)&p);

    if ((int64_t)a <= 0 || ProtectArea(a, AREAR_READ) != 0)
        return 1;
    KLog("writing to a read-only Area");
    p[100] = 1;
    KLog("still running: the Area is writable");
    return 2;
}
