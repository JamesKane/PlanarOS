/*
 * kernel/port.h -- ports and connections (docs/v2-plan.md)
 *
 * A port is a named rendezvous held by a server task. OpenPort makes a
 * connection to it: an arena Area mapped into the client and, once the
 * server accepts it, the server, with a handle and a signal bit for each
 * end. Rings and messages live in the arena, in user memory; the kernel
 * only sets up the arena, passes signals between the ends, and says in
 * the arena when an end has gone.
 *
 * Ports and connection ends belong to tasks: when the task ends, they
 * close. The calls act for the running task, from its kernel call.
 */
#ifndef KERNEL_PORT_H
#define KERNEL_PORT_H

#include <stdint.h>

struct task;

int64_t port_create_call(uint64_t name, int64_t sigbit);
int64_t port_delete_call(uint32_t port);
int64_t port_open_call(uint64_t name, int64_t sigbit, uint64_t slots, uint64_t heap,
                       uint64_t arena_out);
int64_t port_accept_call(uint32_t port, int64_t sigbit, uint64_t arena_out);
int64_t conn_signal_call(uint32_t conn);
uint64_t conn_signal_wait_call(uint32_t conn, uint64_t mask);
int64_t conn_close_call(uint32_t conn);

/* 't' is leaving its process: close its ports and connection ends. */
void ipc_task_gone(struct task *t);

/* Ports and connections in existence, for leak checks. */
unsigned ipc_count(void);

#endif /* KERNEL_PORT_H */
