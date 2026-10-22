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
 * sync object in the client's memory, which the client spins on, or
 * waits on with WaitAddr after GPU_WAIT: the server wakes that word when
 * the GPU says it moved, until it reaches gm_Seqno. Without GPU_WAIT the
 * server isn't told of its updates at all, so a client that spins costs
 * it no interrupt (V6 step 8). To sleep: GPU_WAIT, then look at
 * gs_Seqno, then WaitAddr on what was there.
 *
 * A fault in the client's queue is a message: GPU_EVENT, left with the
 * server, is answered when one comes (V6 step 8). GPU_STATUS still says
 * the last.
 *
 * A GPU with no doorbell for the app (the RV2's Rogue: gm_Doorbell is
 * 0) is a server queue (v5-plan step 6): the client asks GPU_KICK
 * instead of ringing, and the server kicks the GPU. Everything else is
 * the same. What the ring's commands are is the GPU's own (gm_Kind):
 * the CSF's instructions, or Rogue's client CCB commands.
 *
 * Before ringing, the ring's and gq_Insert's writes must reach memory
 * first: drv_io_barrier() (planar/drv.h) orders them, as a driver's.
 *
 * The client's GPU addresses are its own: every Area it lends with
 * GPU_MAP is mapped where GPU_MAP says (gm_Device), and nothing else is
 * there. On the CIX that's its own address; Rogue's firmware sees it in
 * its own heap. The memory must be uncached (AREAF_NC): neither GPU
 * reads or writes it through the CPU's caches.
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
    GPU_OPEN = 1,           /* gm_Task in (the client's); gm_Server, gm_Kind, gm_Id, gm_Cores, gm_Slots out */
    GPU_MAP = 2,            /* gm_Address, gm_Size: an Area the client shared with gm_Server; gm_Device out */
    GPU_UNMAP = 3,          /* gm_Address: one mapped */
    GPU_QUEUE = 4,          /* gm_Ring, gm_RingSize, gm_Sync in; gm_Input, gm_Output, gm_Doorbell out */
    GPU_STATUS = 5,         /* gm_Fault, gm_Faults, gm_Jobs out: the client's queue */
    GPU_KICK = 6,           /* no doorbell: run what's in the ring up to gq_Insert */
    GPU_WAIT = 7,           /* gm_Seqno: wake the sync object as it moves, until it gets there */
    GPU_EVENT = 8,          /* answered when the queue faults: gm_Fault, gm_FaultAddress, gm_Faults */
};

/* GpuMsg.gm_Kind: what the ring's commands are. */
enum : uint32_t {
    GPU_CSF = 1,            /* Mali's CSF instructions, 64 bits each (gpu-panthor) */
    GPU_ROGUE = 2,          /* Rogue's client CCB commands (gpu-rogue) */
};

struct GpuMsg {
    struct Message gm_Msg;
    uint32_t gm_Op;
    uint32_t gm_Kind;               /* GPU_OPEN: GPU_CSF or GPU_ROGUE */
    Handle gm_Task;
    Handle gm_Server;               /* GPU_OPEN: where to share Areas to */
    uint64_t gm_Address, gm_Size;   /* GPU_MAP, GPU_UNMAP */
    uint64_t gm_Ring;               /* GPU_QUEUE: in a mapped Area, gm_RingSize-aligned */
    uint64_t gm_RingSize;           /* a power of two, 4 KiB to 1 MiB */
    uint64_t gm_Sync;               /* a struct GpuSync in a mapped Area: woken as it moves */
    uint64_t gm_Input;              /* GPU_QUEUE: a struct GpuQueueIn, read-write */
    uint64_t gm_Output;             /* a struct GpuQueueOut, read-only */
    uint64_t gm_Doorbell;           /* a page: write 1 to its first word; 0: GPU_KICK */
    uint32_t gm_Id;                 /* GPU_OPEN: the GPU's own ID register */
    uint32_t gm_Slots;              /* queues it can have, and has free */
    uint32_t gm_Free;
    uint32_t gm_Fault;              /* GPU_STATUS: the last fault, 0 if none */
    uint64_t gm_Cores;              /* GPU_OPEN: its shader cores, a bit each */
    uint64_t gm_FaultAddress;
    uint32_t gm_Faults;             /* how many */
    uint32_t gm_Regs;               /* GPU_OPEN: a stream's registers; the last 4 are the app's */
    uint64_t gm_Device;             /* GPU_MAP: where the GPU sees the Area */
    uint64_t gm_Seqno;              /* GPU_WAIT: the value the sleeper waits for */
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

/* A sync object, as the CSF's SYNC_ADD64 and SYNC_SET64 have it. Rogue's
 * firmware writes 32 bits (a UFO): gs_Seqno's low half. */
struct GpuSync {
    volatile uint64_t gs_Seqno;
    volatile uint32_t gs_Status;    /* nonzero: the work before it failed */
    uint32_t gs_Pad;
};

#endif /* PLANAR_DEVICES_GPU_H */
