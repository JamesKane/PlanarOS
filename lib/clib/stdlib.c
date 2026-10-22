/*
 * lib/clib/stdlib.c -- malloc and its family, exit and abort (docs §17.4.5)
 *
 * The memory is AllocVec's: blocks from the process's pool, or an Area
 * of their own past its threshold (lib/exec/pool.c). realloc keeps a
 * block that's big enough already, so a block shrunk keeps its room.
 */
#include <stdckdint.h>

#include <planar/exec.h>

#include <errno.h>
#include <stdlib.h>

#include "../exec/vec.h"

void *malloc(size_t size)
{
    void *p = AllocVec(size ? size : 1, 0);

    if (!p)
        errno = ENOMEM;
    return p;
}

void *calloc(size_t n, size_t size)
{
    size_t total;
    void *p;

    if (ckd_mul(&total, n, size)) {
        errno = ENOMEM;
        return nullptr;
    }
    if (!(p = AllocVec(total ? total : 1, AREAF_CLEAR)))
        errno = ENOMEM;
    return p;
}

void *realloc(void *p, size_t size)
{
    uint64_t have;
    void *q;

    if (!p)
        return malloc(size);
    if ((have = _vec_size(p)) >= size)
        return p;
    if (!(q = malloc(size)))
        return nullptr;                 /* p is still the caller's */
    CopyMem(p, q, have);
    FreeVec(p);
    return q;
}

void free(void *p)
{
    FreeVec(p);
}

size_t memalignment(const void *p)
{
    uintptr_t a = (uintptr_t)p;

    return a & -a;
}

void exit(int status)
{
    ExitProcess(status);
}

void _Exit(int status)
{
    ExitProcess(status);
}

void abort(void)
{
    KLog("abort() called");
    ExitProcess(EXIT_FAILURE);
}

int abs(int j)
{
    return j < 0 ? -j : j;
}

long labs(long j)
{
    return j < 0 ? -j : j;
}

long long llabs(long long j)
{
    return j < 0 ? -j : j;
}
