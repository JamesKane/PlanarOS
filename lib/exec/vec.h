/*
 * lib/exec/vec.h -- what lib/clib needs of AllocVec's blocks
 *
 * Not exec.library's: linked from libexec.a, for realloc.
 */
#ifndef LIB_EXEC_VEC_H
#define LIB_EXEC_VEC_H

#include <stdint.h>

/* The bytes a block from AllocVec has room for: at least what was asked. */
uint64_t _vec_size(const void *memoryBlock);

#endif /* LIB_EXEC_VEC_H */
