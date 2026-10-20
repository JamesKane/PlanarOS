/*
 * kernel/handle.h -- handles: how user code names kernel objects
 *
 * One table for the whole system, so a handle means the same object in
 * every process (Signal crosses processes). A handle is generation << 16
 * | index; freeing an entry bumps its generation, so a stale handle finds
 * nothing instead of a recycled object. From omega's kernel/handle.c,
 * made safe on every CPU.
 */
#ifndef KERNEL_HANDLE_H
#define KERNEL_HANDLE_H

#include <planar/exec.h>

enum handle_type : uint8_t {
    HANDLE_FREE,
    HANDLE_TASK,
    HANDLE_AREA,
};

struct process;

/* A handle for 'obj', owned by 'owner' (nullptr: the kernel). 0 if full. */
Handle handle_new(enum handle_type type, void *obj, struct process *owner);
void handle_free(Handle h);

/*
 * Look up 'h' and, if it's of 'type', run fn(obj, arg) with the table
 * locked, so the object can't be freed under it; returns whether it ran.
 */
bool handle_with(Handle h, enum handle_type type, void (*fn)(void *obj, void *arg), void *arg);

/* Free every handle 'owner' holds. */
void handle_free_owner(struct process *owner);

/* Handles in use, for leak checks. */
unsigned handle_count(void);

#endif /* KERNEL_HANDLE_H */
