/*
 * lib/clib/errno.c -- errno, each task's own (docs §17.4.5)
 */
#include <errno.h>

thread_local int errno;
