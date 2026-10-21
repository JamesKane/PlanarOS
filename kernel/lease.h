/*
 * kernel/lease.h -- device leases (planar/lease.h)
 *
 * A lease is a device's MMIO pages and interrupt lines, created by the
 * holder of the device root and granted to one process at a time. The
 * process code tells this file when a grantee's tasks are told to stop
 * (its lines are masked), when one of its tasks leaves (that task's
 * lines are unbound), and when it is reclaimed (the lease is stopping,
 * and its DMA Areas quarantined as they go). area.c asks it what to do
 * with a DMA Area that nobody maps any more.
 *
 * The calls act for the running task, from its kernel call.
 */
#ifndef KERNEL_LEASE_H
#define KERNEL_LEASE_H

#include <stdint.h>

#include <planar/exec.h>

struct area;
struct lease;
struct process;
struct task;

/* A device root handle for 'p', which may then create leases; 0 if the
 * table is full. The kernel gives one to the first server. */
Handle lease_root(struct process *p);

/* The process ends: mask its lines now (its tasks are stopping), and,
 * once it's reclaimed, mark its leases stopping. */
void lease_process_dying(struct process *p);
void lease_process_died(struct process *p);

/* 't' leaves its process: unbind and mask the lines bound to it. */
void lease_task_gone(struct task *t);

/* The handle of a lease 'owner' owns is gone (CloseHandle or the
 * owner's death): it goes when nothing else needs it. */
void lease_release(struct lease *l);

/*
 * DMA Areas. area.c counts each in with lease_dma_new, and offers it
 * back with lease_dma_gone once nobody maps it: true means the lease
 * took it into quarantine (its grantee died with it unfreed, so the
 * device may still write to it), false that area.c frees it as usual.
 */
void lease_dma_new(struct lease *l, uint64_t pages);
uint64_t lease_dma_limit(const struct lease *l);    /* 0: anywhere */
bool lease_dma_gone(struct lease *l, struct area *a, uint64_t pages, bool freed);

/* Leases in existence, and pages quarantined, for leak checks. */
unsigned lease_count(void);
uint64_t lease_quarantined(void);

/* The kernel calls. */
int64_t lease_create_call(uint32_t root, uint64_t base, uint64_t size, uint64_t irqs,
                          uint32_t nirqs);
int64_t lease_grant_call(uint32_t lease, uint32_t process);
int64_t lease_revoke_call(uint32_t lease);
int64_t lease_query_call(uint32_t lease, uint64_t info_out);
int64_t lease_close_call(uint32_t lease);
int64_t lease_map_call(uint32_t lease, uint64_t addr_out);
int64_t lease_bind_call(uint32_t lease, uint32_t line, int64_t sigbit);
uint64_t lease_ack_wait_call(uint32_t lease, uint32_t line, uint64_t mask);
int64_t lease_dma_call(uint32_t lease, uint64_t size, uint32_t flags, uint64_t addr_out);
int64_t lease_firmware_call(uint32_t root, uint64_t info_out);
int64_t lease_firmware_grant_call(uint32_t root, uint32_t process);
int64_t lease_console_grant_call(uint32_t root, uint32_t process);
int64_t lease_console_read_call(uint64_t buf, uint32_t len);
int64_t lease_share_call(uint32_t lease, uint64_t addr, uint64_t size, uint32_t task);
int64_t lease_unshare_call(uint32_t lease, uint64_t addr, uint32_t task);
int64_t lease_limit_dma_call(uint32_t lease, uint64_t limit);
int64_t lease_set_call_call(uint32_t lease, uint32_t fid);
int64_t lease_call_call(uint32_t lease, uint64_t regs);
int64_t lease_msi_call(uint32_t root, uint32_t device, uint32_t count, uint64_t info_out);

#endif /* KERNEL_LEASE_H */
