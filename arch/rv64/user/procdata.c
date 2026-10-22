/*
 * arch/rv64/user/procdata.c -- the process data, through tp
 *
 * The thread block's first word (lib/rt/tls.c): tp points 64 bytes in,
 * where the thread_local variables start.
 */
#include "../../../lib/exec/procdata.h"

struct ProcessData *process_data(void)
{
    struct ProcessData **block;

    __asm__("addi %0, tp, -64" : "=r"(block));
    return *block;
}
