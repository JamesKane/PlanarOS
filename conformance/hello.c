/*
 * conformance/hello.c -- the smallest program: log a line and exit
 *
 * Exits 7. With the argument "name" (from dostest, which loads it from
 * a disk), it first tries to make a named port, which a program loaded
 * so mustn't (PROCF_NAMES, §10.8): 8 if refused, 9 if it got the name.
 */
#include <planar/exec.h>
#include <planar/ipc.h>

int main(uint64_t arg)
{
    const char *a = (const char *)arg;

    KLog("hello from user mode");
    if (a && a[0] == 'n' && a[1] == 'a' && a[2] == 'm' && a[3] == 'e' && !a[4]) {
        int64_t port = CreatePort("dos", 16);

        if (port >= 0)
            DeletePort((Handle)port);
        return port == ERR_RIGHTS ? 8 : 9;
    }
    return 7;
}
