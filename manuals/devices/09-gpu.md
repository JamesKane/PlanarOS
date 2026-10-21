# The GPU class

*Checked against V5 step 5.*

## What it's for

Work for a coprocessor, run from a queue the app holds itself ([docs §5.1](../../docs/05-coprocessors.md)). A GPU server publishes `gpu.0`. A client lends it memory, takes a queue, and from then on submits work with no kernel call and no message: it writes commands into its ring, moves the ring's insert pointer on, and writes 1 to a doorbell page of its own. The GPU says it's done by adding to a **sync object** in the client's memory, which the client spins on or waits on with `WaitAddr`.

`gpu-panthor` (`drv/gpu-panthor`) is the CIX's server: its Mali-G720, through the CSF (command stream frontend) firmware. Linux's panthor writes the ring and rings the doorbell for every job, in the kernel; Planar gives the app the queue. The RV2's GPU, which has no user doorbell, is step 6's `gpu-rogue`, with the same API through a server queue.

V5's jobs are the smallest a CSF can be seen to run: add 1 to a sync object. Compute shaders and graphics are V6's.

## Concepts

- **Its own address space.** Each client has a GPU address space of its own. Every Area it lends (`GPU_MAP`) is mapped there at its own address, the identity layout's, and nothing else is. A job that reaches elsewhere faults, and only that client's queue stops.
- **Uncached memory.** What the GPU reads and writes must be `AREAF_NC`: the G720 is mapped non-cacheable to the CPU's memory, without snooping (the CIX patches' rule for a GPU with no IOMMU that isn't fully coherent). `GPU_MAP` refuses a cached Area (`ERR_RIGHTS`).
- **Lending.** The client shares the Area with the server (`ShareArea` to `gm_Server`, read-write), then asks `GPU_MAP` with its address and size: the whole Area. The server lets it go (`ReleaseArea`) at `GPU_UNMAP`, or when the client goes.
- **A queue** is one group on one of the CSF's group slots, pinned there for as long as it stands, with one stream. Slot *n* has address space *n* + 1 and doorbell *n* + 1, so the CIX has 7 queues to give, one per client. `GPU_QUEUE` gives three addresses:
  - `gm_Input`: a `struct GpuQueueIn` page, read-write: `gq_Insert`, the bytes ever written to the ring.
  - `gm_Output`: a `struct GpuQueueOut` page, read-only: `gq_Extract`, the bytes the GPU has read, which the firmware moves when it gets to it, not at each job.
  - `gm_Doorbell`: the page to write 1 to.
- **The ring** is the client's own memory, in a mapped Area: a power of two from 4 KiB to 1 MiB, aligned to its size (an Area is only page aligned: allocate twice the size and align inside it). Commands are 64-bit CSF instructions, a 64-byte line at a time.
- **Registers.** A stream has `gm_Regs` work registers (from `GPU_OPEN`); the last 4 are free for the app's instructions, as panthor uses them.
- **Waking.** `gm_Sync` names a sync object: when the firmware says a sync object of the queue moved (`CSG_SYNC_UPDATE`), the server wakes that word with `WakeAddr`. Spinning needs no server at all.
- **Faults.** An MMU fault, or a stream's fault or fatal error, stops the client's queue (its group terminated) and is kept: `GPU_STATUS` gives the last (`gm_Fault`, `gm_FaultAddress`) and the count. The firmware's own faults give the GPU up until the server restarts.
- **Going.** When the client's connection goes, dead or not, its group is terminated, its address space taken down, and what it lent released, in that order. When the server dies, hwdb stops the GPU (the MCU off, a soft reset) before the server's DMA Areas, and what clients lent it, leave quarantine.

## Using it

From `conformance/gputest.c`, shortened:
```c
struct MsgPort mp;
struct GpuMsg *m;
void *area, *sync;
uint64_t *ring;
struct AreaInfo ai;

ConnectPort(&mp, "gpu.0", 20, 4, 2 * sizeof *m);
m = AllocMsg(&mp, sizeof *m);
memset(m, 0, sizeof *m);
m->gm_Msg.mn_Length = sizeof *m;
m->gm_Op = GPU_OPEN;
m->gm_Task = FindTask(nullptr);
DoMsg(&mp, &m->gm_Msg);                 /* gm_Server, gm_Regs; ERR_BUSY while its firmware loads */

AllocArea(2 * RING, AREAF_NC, &area);   /* a 64 KiB ring, aligned in it */
ring = (uint64_t *)(((uint64_t)area + RING - 1) & ~(RING - 1));
QueryArea(area, &ai);
ShareArea(ai.ai_Area, m->gm_Server, AREAR_READ | AREAR_WRITE);
m->gm_Op = GPU_MAP;
m->gm_Address = (uint64_t)area;
m->gm_Size = 2 * RING;
DoMsg(&mp, &m->gm_Msg);
/* ... the same for the sync object's Area ... */

m->gm_Op = GPU_QUEUE;
m->gm_Ring = (uint64_t)ring;
m->gm_RingSize = RING;
m->gm_Sync = (uint64_t)sync;
DoMsg(&mp, &m->gm_Msg);                 /* gm_Input, gm_Output, gm_Doorbell */
```
A job, then, with `a` and `v` the last register pairs (`gm_Regs - 4`, `gm_Regs - 2`):
```c
struct GpuQueueIn *in = (struct GpuQueueIn *)m->gm_Input;
uint64_t *j = &ring[in->gq_Insert % RING / 8];

j[0] = 1ull << 56 | a << 48 | (uint64_t)sync;   /* MOV48 ra, the sync object */
j[1] = 1ull << 56 | v << 48 | 1;                /* MOV48 rv, 1 */
j[2] = 51ull << 56 | a << 40 | v << 32 | 1;     /* SYNC_ADD64 [ra], rv */
j[3] = 47ull << 56;                             /* ERROR_BARRIER */
j[4] = j[5] = j[6] = j[7] = 0;                  /* NOP, to the line's end */
drv_io_barrier();
in->gq_Insert += 64;
drv_io_barrier();
*(volatile uint32_t *)m->gm_Doorbell = 1;
while (((struct GpuSync *)sync)->gs_Seqno != seqno)
    ;                                           /* or WaitAddr on it */
```
The encodings are panthor's job epilogue (`panthor_sched.c`, `prepare_job_instrs`).

## Rules

- Lend whole Areas, `AREAF_NC`, shared read-write with the server first.
- Write the ring, then `gq_Insert`, then the doorbell, with `drv_io_barrier()` (`planar/drv.h`) between: the GPU reads them without the CPU's caches, and the doorbell is a device write.
- Keep at most the ring's size unread (`gq_Insert` − `gq_Extract`); the GPU reads round it.
- `GPU_UNMAP` is refused (`ERR_BUSY`) while the client has a queue: it may be using the memory.
- One queue a client.

## The driver

`gpu-panthor`'s three windows are the GPU's RCSU (`0x15000000`), its registers (`0x15010000`, doorbells from `+0x80000`), and RST0's page (`0x16000000`, a hwdb quirk). Its three lines are the job's, the MMU's and the GPU's (`every_line`). Nothing is read from the GPU until it has power, so its bind only takes the device; the rest is done from the loop:

1. power's set `"gpu"` (SCMI domain 21 and two clocks) is on: hwdb votes for it before the lease (`HW_Q_POWER`), so it stays on until a dead driver's GPU has been stopped.
2. GPUP's power resource, as the DSDT's `_ON` does it: bits 2–12 of the RCSU's `0x218`, and the memory repair if RST0's strap asks for it.
3. A pulse of the GPU's reset, RST0 `0x400` bit 6, held 20 ms. UEFI leaves it held: until it's released, the first read of a GPU register is refused by the interconnect and the CPU takes an SError. Not the RCSU's reset (`0x800` bit 28): pulsing that takes away the non-secure access TF-A gave the RCSU.
4. The Q-channel's clock gating (`0x218` bit 0), a soft reset, and the features: shader cores, less those the RCSU says are harvested.
5. The firmware, `mali_csffw.bin`, read through `dos` from `SYS:EFI/planar/DEVS/` once dos is there. Its sections are laid into the MCU's address space (0) at the addresses it asks for; the L2 on, with ACE-Lite coherency; the MCU started; then the global interface's timers, cores and interrupts.
6. A ping every 12 s, the firmware's watchdog.

The page tables are AArch64 stage 1, 4 KiB pages, from a pool of uncached tables, walked uncached. Every attribute is non-cacheable.

## The machines

**The CIX** (2026-10-21): `GPU_ID 0xc8700008` (arch 12), 10 shader cores (`0x550555`), 8 address spaces, 48-bit VA. The firmware's global interface is version 1.1, with 8 group slots: 7 queues for clients. Measured by `gputest`, three rounds a boot:
- the first job, doorbell to the sync object's update: 25–50 µs, with 2 kernel calls made meanwhile machine-wide, both the test's own (`QuerySystem` and `CurrentTime`);
- 1,000 jobs, each spun on: about 11 µs each;
- one job waited for with `WaitAddr`, woken by the server: 16 µs.

**QEMU** has no GPU: the test says "not exercised".

**The RV2**: step 6.

## Functions used

- [ShareArea](../autodocs/exec.md#execsharearea), [QueryArea](../autodocs/exec.md#execqueryarea), [WaitAddr](../autodocs/exec.md#execwaitaddr), [ConnectPort](../autodocs/msg.md#execconnectport), [DoMsg](../autodocs/msg.md#execdomsg): the client's side.
- [ReleaseArea](../autodocs/exec.md#execreleasearea), [ShareLease](../autodocs/lease.md#leasesharelease), [AllocDmaArea](../autodocs/lease.md#leaseallocdmaarea), [WakeAddr](../autodocs/exec.md#execwakeaddr): the server's.
