/*
 * kernel/library.h -- the shared library region (V6 step 4)
 *
 * Every library in the boot bundle (planar/libimage.h), relocated once
 * at boot into one Area, which every process maps read and execute,
 * global, at the same address. The region is never freed.
 */
#ifndef KERNEL_LIBRARY_H
#define KERNEL_LIBRARY_H

#include <stdint.h>

struct process;

/* Build the region, before the first process. False, logged, if a
 * library is malformed: then there's no region, and IExec is nullptr. */
bool library_init(void);

/* Map the region into a new process: false if it couldn't be. True,
 * mapping nothing, if there's no region. */
bool library_map(struct process *p);

/* exec.library's main interface, the address every process starts with
 * in its third argument; 0 without one. */
uint64_t library_iexec(void);

/* The region's pages and the libraries in it, for the self-test. */
uint64_t library_pages(void);
unsigned library_count(void);
bool library_vector(void);          /* exec's CopyMem, by the vector unit */

#endif /* KERNEL_LIBRARY_H */
