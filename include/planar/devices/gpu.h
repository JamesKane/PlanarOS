/*
 * planar/devices/gpu.h -- the GPU class: a queue of the app's own
 * (v5-plan step 5, §5.1)
 *
 * A GPU server publishes "gpu.0". A client lends it memory and gets a
 * queue: a ring of commands in its own memory, two pages saying how
 * far it has written and how far the GPU has read, and on the CIX a
 * doorbell page of its own. To run work it writes commands into the
 * ring, moves gq_Insert on, and writes 1 to its doorbell: no kernel
 * call and no message. The work says when it's done by adding to a
 * sync object in the client's memory, which the client spins on or
 * waits on with WaitAddr: the server wakes that word when the GPU says
 * a sync object moved.
 *
 * Before ringing, the ring's and gq_Insert's writes must reach memory
 * first: drv_io_barrier() (planar/drv.h) orders them, as a driver's.
 *
 * The client's GPU addresses are its own: every Area it lends with
 * GPU_MAP is mapped at its own address, and nothing else is there. The
 * memory must be uncached (AREAF_NC): the G720 reads and writes it
 * without the CPU's caches.
 *
 * Its queue, its mappings and its slot go with its connection, dead or
 * not; and all of them with the server.
 */
#ifndef PLANAR_DEVICES_GPU_H
#define PLANAR_DEVICES_GPU_H

#include <stdint.h>

#include <planar/exec.h>
#include <planar/msg.h>

/* GpuMsg.gm_Op */
enum : uint32_t {
    GPU_OPEN = 1,           /* gm_Task in (the client's); gm_Server, gm_Id, gm_Cores, gm_Slots out */
    GPU_MAP = 2,            /* gm_Address, gm_Size: an Area the client shared with gm_Server */
    GPU_UNMAP = 3,          /* gm_Address: one mapped */
    GPU_QUEUE = 4,          /* gm_Ring, gm_RingSize, gm_Sync in; gm_Input, gm_Output, gm_Doorbell out */
    GPU_STATUS = 5,         /* gm_Fault, gm_Faults, gm_Jobs out: the client's queue */
};

struct GpuMsg {
    struct Message gm_Msg;
    uint32_t gm_Op;
    uint32_t gm_Pad;
    Handle gm_Task;
    Handle gm_Server;               /* GPU_OPEN: where to share Areas to */
    uint64_t gm_Address, gm_Size;   /* GPU_MAP, GPU_UNMAP */
    uint64_t gm_Ring;               /* GPU_QUEUE: in a mapped Area, gm_RingSize-aligned */
    uint64_t gm_RingSize;           /* a power of two, 4 KiB to 1 MiB */
    uint64_t gm_Sync;               /* a struct GpuSync in a mapped Area: woken as it moves */
    uint64_t gm_Input;              /* GPU_QUEUE: a struct GpuQueueIn, read-write */
    uint64_t gm_Output;             /* a struct GpuQueueOut, read-only */
    uint64_t gm_Doorbell;           /* a page: write 1 to its first word */
    uint32_t gm_Id;                 /* GPU_OPEN: the GPU's own ID register */
    uint32_t gm_Slots;              /* queues it can have, and has free */
    uint32_t gm_Free;
    uint32_t gm_Fault;              /* GPU_STATUS: the last fault, 0 if none */
    uint64_t gm_Cores;              /* GPU_OPEN: its shader cores, a bit each */
    uint64_t gm_FaultAddress;
    uint32_t gm_Faults;             /* how many */
    uint32_t gm_Regs;               /* GPU_OPEN: a stream's registers; the last 4 are the app's */
};

/* The pages GPU_QUEUE gives: the CSF's ring-buffer interfaces. */
struct GpuQueueIn {
    volatile uint64_t gq_Insert;    /* bytes written into the ring, ever */
    volatile uint64_t gq_Extract;   /* the server's */
};

struct GpuQueueOut {
    volatile uint64_t gq_Extract;   /* bytes the GPU has read, ever */
    volatile uint32_t gq_Active;
};

/* A sync object, as the CSF's SYNC_ADD64 and SYNC_SET64 have it. */
struct GpuSync {
    volatile uint64_t gs_Seqno;
    volatile uint32_t gs_Status;    /* nonzero: the work before it failed */
    uint32_t gs_Pad;
};

#endif /* PLANAR_DEVICES_GPU_H */
