# drv: drv.library

*Checked against V5.*

What a driver links to do its job, in alphabetical order. The header is `planar/drv.h`; the code is `lib/drv/drv.c`, linked into every program under `drv/` (`mk/user.mk`). None is a kernel call. How a driver is written with them is [Devices ch. 1](../devices/01-drivers.md).

---

## drv/drv_dma_clean

**NAME**
drv_dma_clean, drv_dma_flush -- DMA memory and the caches

**SYNOPSIS**
```c
#include <planar/drv.h>
void drv_dma_clean(const void *p, uint64_t n);
void drv_dma_flush(const void *p, uint64_t n);
```

**FUNCTION**
For a device that doesn't snoop the CPU's caches (the K1's, and the CIX's display and USB controllers). `drv_dma_clean` writes the cached lines of `p` to `p + n` back to memory: call it before the device reads them (descriptors, data to write). `drv_dma_flush` writes them back and drops them: call it before the device writes a buffer and again before reading what it wrote. Whole cache blocks are done, so keep a device's buffers from sharing a block with anything else.

On RISC-V they're Zicbom's `cbo.clean` and `cbo.flush`, which the kernel lets user mode run. On Arm they're `dc cvac` and `dc civac` by the cache's line (`CTR_EL0`), which the kernel lets EL0 run too (`SCTLR_EL1.UCI` and `UCT`). Where the device snoops they cost a little and change nothing.

**SEE ALSO**
`drv/sdhci/main.c`, `drv/display/main.c`; [Devices ch. 5](../devices/05-drivers.md)

---

## drv/drv_hwdb

**NAME**
drv_hwdb_msg, drv_hwdb -- a bus driver's messages to hwdb

**SYNOPSIS**
```c
#include <planar/drv.h>
struct HwdbMsg *drv_hwdb_msg(struct drv *d, uint32_t op);
int64_t drv_hwdb(struct drv *d, struct HwdbMsg *m);
```

**FUNCTION**
For a bus driver's Bind, which tells hwdb about what it found. `drv_hwdb_msg` gives the driver's one spare message, cleared, with `op` and the driver's token filled in. `drv_hwdb` sends it and waits for hwdb's answer.

**RESULT**
`drv_hwdb_msg`: the message, or `nullptr` if there's no heap for it. `drv_hwdb`: 0, or the error hwdb or the connection gave.

**EXAMPLE**
From `drv/pci/main.c`:
```c
struct HwdbMsg *m = drv_hwdb_msg(d, HWDB_PCI_HOST);

m->hm_Host.ph_Device = (uint32_t)(dev - d->dev);
if (drv_hwdb(d, m) != 0)
    return -11;
host = m->hm_Host;
```

**NOTES**
The message is reused: copy what you need from an answer before the next call.

**SEE ALSO**
[Devices ch. 4](../devices/04-pci.md)

---

## drv/drv_in_heap

**NAME**
drv_in_heap -- is a client's buffer where it may be?

**SYNOPSIS**
```c
#include <planar/drv.h>
bool drv_in_heap(const struct drv *d, unsigned c, uint64_t addr, uint64_t size);
```

**FUNCTION**
True if `size` bytes at `addr` lie in client `c`'s connection heap, the only memory of the client's a driver may hand its device. A block driver checks every request's buffer with it.

**BUGS**
None known.

---

## drv/drv_io_barrier

**NAME**
drv_io_barrier -- memory a device uses, ordered against its registers

**SYNOPSIS**
```c
#include <planar/drv.h>
void drv_io_barrier(void);
```

**FUNCTION**
Every memory access before it, cached or uncached (`AREAF_NC`), is done before any device register access after it, and the other way round. Call it between writing what a device will read (a ring's entries) and the register that tells it to (a doorbell), and between seeing in memory that the device wrote something (an event's cycle bit) and reading the rest. A compiler's `__atomic_thread_fence` isn't enough for either: on RISC-V it doesn't order I/O, and on Arm it orders only within the inner shareable domain.

It's `fence iorw, iorw` on RISC-V and `dsb sy` on Arm.

**SEE ALSO**
`drv/xhci/hc.c`; [drv_dma_clean](#drvdrv_dma_clean)

---

## drv/drv_lease

**NAME**
drv_lease, drv_wait_irq -- the lease, and waiting for a line

**SYNOPSIS**
```c
#include <planar/drv.h>
Handle drv_lease(const struct drv *d);
uint64_t drv_wait_irq(struct drv *d, struct drv_dev *dev, uint64_t ms);
```

**FUNCTION**
`drv_lease` gives the first range's lease, which carries the lines: the one to give `AllocDmaArea`. `drv_wait_irq` waits for `dev`'s line, at most `ms` milliseconds (0: no limit), and acknowledges it (`AckWaitIrq`). For Bind, before the library's loop runs.

**RESULT**
`drv_wait_irq`: the signals that came; the line's is signal 32 plus its index.

**BUGS**
None known.

---

## drv/drv_log

**NAME**
drv_log -- a line in the kernel's log

**SYNOPSIS**
```c
#include <planar/drv.h>
void drv_log(const struct drv *d, const struct drv_dev *dev, const char *text);
```

**FUNCTION**
Logs `text`, after the driver's name and, if `dev` isn't `nullptr`, its unit: "nvme.0: ...".

**BUGS**
None known.

---

## drv/drv_main

**NAME**
drv_main -- a driver's main loop

**SYNOPSIS**
```c
#include <planar/drv.h>
[[noreturn]] void drv_main(uint64_t token, const struct drv_ops *ops);
```

**FUNCTION**
Runs a driver, given its token (its `main`'s argument) and its callbacks:
1. Connects to "hwdb", asks for its devices (`HWDB_BIND`), checks the answer and maps every range of registers where it is. A device may have no registers (`dd_Size` 0, and no line): QEMU's power model, which hwdb's test mode adds, is leased no range at all.
2. Calls `bind` for each device: `DRV_OK` to take it, `DRV_DECLINE` (not one this driver drives), or a negative error, which ends the process. Binds each taken device's line, or its MSI-X lines (`dd_NLines`), or, if `every_line` is set, every line its description gives it (`dd_NLines` from `dd_Line` on: the G720's job, MMU and GPU lines). `interrupt` is called once for any of a device's lines; it reads the device to see which.
3. Calls `start` for each taken device, to publish its class port (`drv_publish`).
4. Tells hwdb what it took and published (`HWDB_STARTED`). A driver that took nothing exits 0.
5. Loops: accepts clients at the class ports (8 a port, `DRV_CLIENTS`), calls `interrupt` when a device's line fires and acknowledges it, and gives each client message to `request` (answered when it returns) or `message` (answered with `drv_reply`). With a `poll` callback that says work is outstanding, it looks again without sleeping. With a `deadline` callback that gives a time, it sleeps until then at most and calls `poll` (a command whose interrupt may not come).

When a client's connection goes, by `CloseConn` or its death, the loop calls `gone` with its number before closing it: what the driver holds for that client is to go (§11.5, provider state per connection). power drops the client's votes there.

It exits when hwdb's connection goes.

**INPUTS**
- `token` -- `main`'s argument, from hwdb.
- `ops` -- `name`, `bind`, `start`, `interrupt`, `request`, `message` with `message_size`, `poll`, `deadline`, `gone`, and the flag `every_line`. Any but `name` and `bind` may be `nullptr`.

**EXAMPLE**
From `drv/nvme/main.c`:
```c
static const struct drv_ops ops = {
    .name = "nvme", .bind = bind, .start = start, .interrupt = interrupt, .message = message,
    .message_size = sizeof(struct BlkMsg), .poll = poll,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
```

**NOTES**
- Signals 16 to 19 are the library's; line *n* of the lease is signal 32 + *n*.
- A client that goes with messages unanswered keeps its connection, out of the loop, until the driver has answered them all: its heap holds buffers the device may be using.
- With `DRVF_CRASH` in hwdb's answer it faults on purpose before Bind: the kill test's give-up case.
- `DRVF_AGAIN` in `d->info.hm_Flags` says a process of this unit ran before: the device is as the last one left it, after hwdb's stop method. display keeps the screen it finds then, rather than clearing it.

**SEE ALSO**
[Devices ch. 1](../devices/01-drivers.md)

---

## drv/drv_publish

**NAME**
drv_publish, drv_publish_as -- a device's class port

**SYNOPSIS**
```c
#include <planar/drv.h>
int64_t drv_publish(struct drv *d, struct drv_dev *dev, const char *cls);
int64_t drv_publish_as(struct drv *d, struct drv_dev *dev, const char *name);
```

**FUNCTION**
For Start: creates `dev`'s port, named for its class and unit: "nvme.0", "sd.0", "con.0", "rng.0". The unit is the device's number among its driver's, fixed when hwdb matched it, so a restarted driver republishes the same name.

`drv_publish_as(d, dev, name)` is the same with the name as given, for a server there is one of: power publishes "power".

A device may publish two ports (`DRV_PORTS`), each a class of its own: display publishes "con.0" and "display.0" on one device. Their clients' messages come to the same callbacks; the driver tells them apart by what they ask. hwdb is told the first.

**RESULT**
0, `ERR_FULL` past `DRV_PORTS`, or what `CreatePort` gives.

**BUGS**
None known.

---

## drv/drv_reply

**NAME**
drv_reply -- answer a client's message

**SYNOPSIS**
```c
#include <planar/drv.h>
void drv_reply(struct drv *d, unsigned c, struct Message *m);
```

**FUNCTION**
Answers `m`, which the `message` callback was given from client `c`, now or later. Each message is answered exactly once. When a client that went has had all its messages answered, its connection closes.

**BUGS**
None known.

**SEE ALSO**
[drv_main](#drvdrv_main), [Devices ch. 2](../devices/02-block.md)
