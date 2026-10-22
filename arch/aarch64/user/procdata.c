/*
 * arch/aarch64/user/procdata.c -- the process data, through TPIDR_EL0
 *
 * The thread block's first word (lib/rt/tls.c), where the thread
 * pointer points: variant I's TCB, before the thread_local variables.
 */
#include "../../../lib/exec/procdata.h"

struct ProcessData *process_data(void)
{
    struct ProcessData **block;

    __asm__("mrs %0, tpidr_el0" : "=r"(block));
    return *block;
}
