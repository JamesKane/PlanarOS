/*
 * clib/errno.h -- C's errno (docs §17.4.5)
 *
 * Each task's own: thread_local, in its block (lib/rt/tls.c). Only
 * lib/clib sets it. Planar's own calls return their errors instead
 * (ERR_*, planar/syscalls.h), and never touch it.
 */
#ifndef CLIB_ERRNO_H
#define CLIB_ERRNO_H

#define ENOMEM  12
#define EINVAL  22
#define EDOM    33
#define ERANGE  34
#define EILSEQ  84

extern _Thread_local int errno;

#endif /* CLIB_ERRNO_H */
