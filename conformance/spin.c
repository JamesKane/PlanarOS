/*
 * conformance/spin.c -- three tasks that spin in user mode forever; the
 * kernel must still be able to kill the process, on whatever CPUs they
 * run on
 */
#include <planar/exec.h>

static void spin(uint64_t)
{
    for (;;)
        ;
}

int main(void)
{
    CreateTask(spin, 0);
    CreateTask(spin, 1);
    KLog("spinning");
    spin(2);
    return 0;
}
