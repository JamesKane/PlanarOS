/*
 * arch/host/arch.c -- the host process as an architecture
 */
#include <stdio.h>
#include <stdlib.h>

#include "arch.h"

const char arch_name[] = "host";

void arch_early_init(void)
{
}

const char *arch_privilege(void)
{
    return "user process";
}

/* A halted host "CPU" is a finished process. */
[[noreturn]] void arch_halt(void)
{
    fflush(stdout);
    exit(0);
}
