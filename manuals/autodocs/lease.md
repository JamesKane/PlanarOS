# lease: device leases

*Checked against V4.*

The calls a driver and its supervisor use, in alphabetical order, with the serial console's input and MSIs, which the device root also governs. The header is `planar/lease.h`. Every one is a kernel call: one stub in `arch/<isa>/user/syscalls.S`, trapping on every call. The code is `kernel/lease.c`, and the design is in [docs §11.4](../../docs/11-driver-model.md) and [V4 step 1](../../docs/v4-plan.md).

A **lease** is one device's registers, whole pages of MMIO, and up to 32 (`LEASE_IRQS`) interrupt lines. Only the holder of the **device root** may create one. The device root is a handle the kernel gives the first server, hwdb, as `main`'s second argument (`process_create_root` in `kernel/process.c`). The kernel's self-test gives it to `conformance/leaser.c`, which plays hwdb, and then to hwdb itself (`servers/hwdb/`). A lease is never made over RAM or reserved memory, over the registers the kernel drives itself, over lines the kernel uses, or over another lease.

The process that creates a lease owns it. It grants the lease to one process at a time, one it created: the driver. The driver maps the registers at their physical address, binds lines to signals, and allocates the device's DMA memory against the lease. The driver doesn't own the lease's handle. It names the lease by the owner's handle value, which the owner passes it, in `CreateProcess`'s `arg` in the tests.

A lease goes round one cycle:

| `li_State` | Meaning | Next |
|---|---|---|
| `LEASES_IDLE` | Not granted. The owner may map the registers. | `GrantLease` makes it granted. `CloseHandle` ends it. |
| `LEASES_GRANTED` | One process, the grantee, holds it. | The grantee's death makes it stopping. |
| `LEASES_STOPPING` | The grantee is dead. The device may still be running. | `RevokeLease`, once the owner has stopped the device, makes it idle. |

There is no call that takes a lease from a live grantee: unbind is kill (§11.4). To take a lease back, end the driver with `KillProcess` or `CloseHandle`, wait for its death signal, stop the device, and call `RevokeLease`.

When the grantee dies:
- Its lines are masked at once, when its tasks are told to stop, before anything else of it goes.
- When it is reclaimed, its lines are unbound, its mapping of the registers goes, and the lease becomes stopping.
- The DMA Areas it hadn't freed with `FreeArea` are **quarantined**. They aren't freed, because the device may still write to them. They stay allocated, counted in `li_QuarantinedPages`, until `RevokeLease`. The owner may map the registers while the lease is stopping, to stop the device first.
- **Lent memory is held too.** When its tasks are told to stop, every Area the grantee maps but doesn't own, and that isn't a DMA Area, is held on the first of its leases: a client's connection arena, whose buffers the device may be reading or writing (the block class, [Devices ch. 2](../devices/02-block.md)). A held Area isn't freed, whoever lets go of it, until `RevokeLease`. It's counted in `li_LentPages` (`area_hold_lent` in `kernel/area.c`).

A bound line reaches the task that bound it as a signal. The kernel masks a level-triggered line each time it fires, so it can't storm before the driver has dealt with it, and `AckWaitIrq` unmasks it. An MSI (an LPI, from `AllocMsi`) is edge-triggered, so the kernel doesn't mask it and `AckWaitIrq` has nothing to unmask.

A lease in the wrong state for a call gives `ERR_BUSY`.

---

## lease/AckWaitIrq

**NAME**
AckWaitIrq -- unmask an interrupt line, then wait for signals

**SYNOPSIS**
```c
#include <planar/lease.h>
uint64_t AckWaitIrq(Handle lease, uint32_t line, uint64_t mask);
```

**FUNCTION**
If `line` has fired since it was bound or last acknowledged, unmasks it at the interrupt controller. An MSI (an LPI) was never masked, so there's nothing to do for one. Then, if `mask` isn't 0, waits as `Wait(mask)` does. With `mask` 0 it returns at once.

Only the task that bound the line may call it.

A kernel call.

**INPUTS**
- `lease` -- the lease's handle, granted to the calling process.
- `line` -- the line's index in the lease, 0 to `li_Irqs` - 1. Not the controller's source number.
- `mask` -- the signals to wait for, usually including the line's. The kernel's own bits are removed.

**RESULT**
With a nonzero `mask`: the signals in `mask` that ended the wait, cleared, as for `Wait`. 0, without unmasking or waiting, if the call is refused.

With `mask` 0: 0, or `(uint64_t)ERR_BADHANDLE` if the call is refused.

The call is refused if `lease` isn't granted to the calling process, `line` is out of range, the line isn't bound, another task bound it, or the process is dying.

**EXAMPLE**
Waiting for a virtio device, with a timeout, after `conformance/virtiorng.c`:
```c
ArmTimer(CurrentTime() + 1000 * MS, 21);
do {
    got = AckWaitIrq(lease, 0, 1ULL << 20 | 1ULL << 21);
    if (got == 0 || (got & 1ULL << 21))
        return 1;                               /* refused, or timed out */
    wr(INTERRUPT_ACK, rd(INTERRUPT_STATUS));    /* quiet the device */
} while (q->used_idx == 0);
ArmTimer(0, 0);
```

**NOTES**
- Quiet the device before the next `AckWaitIrq`. A level-triggered line still asserted when it is unmasked fires again at once.
- Unmasking a line that hasn't fired does nothing, so the first call only waits.
- `Wait` sees the line's signal too, but the line stays masked until `AckWaitIrq`.
- With `mask` 0 it doesn't sleep, unlike `Wait(0)`, which sleeps until the process is killed.

**BUGS**
- Errors are reported only when `mask` is 0. With a nonzero `mask` a refused call returns 0 at once, as `SignalWaitConn` does for a bad handle. A loop on `AckWaitIrq` must treat 0 as an error, or it spins.
- The result is `uint64_t`, so the error arrives as a large unsigned value. Compare with `(uint64_t)ERR_BADHANDLE`.

**SEE ALSO**
[BindIrq](#leasebindirq), [Wait](exec.md#execwait), [ArmTimer](exec.md#execarmtimer)

---

## lease/AllocDmaArea

**NAME**
AllocDmaArea -- allocate a device's DMA memory

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t AllocDmaArea(Handle lease, uint64_t size, uint32_t flags, void **address);
```

**FUNCTION**
Allocates an Area as `AllocArea` does, counted against `lease`. The calling process owns the Area. Its address is its physical address and it is one physically contiguous run, so the address can be given to the device as it is.

Free it with `FreeArea` once the device is done with it. If the grantee dies without freeing it, it is quarantined instead, until `RevokeLease`.

A kernel call.

**INPUTS**
- `lease` -- the lease's handle, granted to the calling process.
- `size` -- bytes wanted; more than 0. Rounded up to whole pages.
- `flags` -- as for `AllocArea`. `AREAF_DMA32` places it below 4 GiB, for a device whose DMA addresses are 32 bits (SDHCI's ADMA2).
- `address` -- where to store the Area's address.

**RESULT**
The Area's handle (positive), or:
- `ERR_BADHANDLE` -- `lease` isn't granted to the calling process, or another task of the process freed the new handle before this call returned.
- `ERR_BADARG` -- `size` is 0, or `flags` has an unknown bit.
- `ERR_NOMEM` -- out of memory, no run of free pages is long enough (below 4 GiB with `AREAF_DMA32`), or the system's handle table is full.
- `ERR_LIMIT` -- the calling process already holds 256 handles.
- `ERR_BADADDR` -- `address` isn't writable. The Area has been freed again.

**NOTES**
- Its pages count in `li_DmaPages` while it lives.
- `FreeArea` is the driver's word that the device has finished with the Area. The kernel can't check it, and frees the Area as usual once nobody maps it.
- Quarantine is decided when the Area's last mapping goes. An Area the driver shared and didn't free is quarantined only if the lease isn't idle when the other process lets it go (`lease_dma_gone` in `kernel/lease.c`).
- The Area work is in `kernel/area.c` (`area_alloc_dma`).

**BUGS**
If the kernel can't allocate a quarantine record, the Area leaks rather than being quarantined, and the kernel log says so. It is never reused.

**SEE ALSO**
[RevokeLease](#leaserevokelease), [QueryLease](#leasequerylease), [AllocArea](exec.md#execallocarea), [FreeArea](exec.md#execfreearea)

---

## lease/AllocMsi

**NAME**
AllocMsi -- give a PCI function its MSIs

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t AllocMsi(Handle root, uint32_t device, uint32_t count, struct MsiInfo *info);
```

**FUNCTION**
Makes `count` interrupt sources for the device whose MSI writes carry device ID `device`, its events 0 to `count - 1`, and fills in `info`:

| Field | |
|---|---|
| `mi_Doorbell` | The address the device writes an event's number to (the ITS's `GITS_TRANSLATER`) |
| `mi_First` | The source event 0 raises; event `i` raises `mi_First + i` |
| `mi_Count` | `count` |

The sources are LPIs, 8192 and up. A lease may then carry them as lines, and the driver binds them with `BindIrq` as any other. The device keeps them for the boot: there is no call to free them.

hwdb calls it for each PCI function with MSI-X ([Devices ch. 4](../devices/04-pci.md)). The device ID is the function's requester ID as its bridge's `msi-map` or the IORT translates it.

A kernel call.

**INPUTS**
- `root` -- the device root handle, owned by the calling process.
- `device` -- the MSI device ID.
- `count` -- 1 to 32 (`LEASE_IRQS`).
- `info` -- where to store what was made.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `root` isn't a device root the calling process owns.
- `ERR_BADARG` -- `count` is 0 or over 32.
- `ERR_NOTFOUND` -- the interrupt controller has no MSIs at all (a PLIC).
- `ERR_EXISTS` -- `device` was given its sources already.
- `ERR_FULL` -- 64 devices have theirs, there's no ITS in use, or it has no room: no LPIs left (1,024 in all), or its device table can't take the ID.
- `ERR_BADADDR` -- `info` isn't writable.

**NOTES**
- An LPI is edge-triggered. The kernel doesn't mask it as it fires, and `AckWaitIrq` has nothing to unmask.
- An LPI goes to the CPU of the task that last enabled it: `BindIrq` moves it there (`MOVI`, `arch/aarch64/its.c`).
- The ITS's tables for the device (its ITT, a device-table page) stay for the boot. The kernel's self-test counts them apart (`intc_msi_pages`).

**BUGS**
After `ERR_BADADDR` the sources are made and the device counts as given, but the caller doesn't know them.

**SEE ALSO**
[CreateLease](#leasecreatelease), [BindIrq](#leasebindirq), [Devices ch. 4](../devices/04-pci.md)

---

## lease/BindIrq

**NAME**
BindIrq -- deliver an interrupt line as a signal

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t BindIrq(Handle lease, uint32_t line, int64_t sigbit);
```

**FUNCTION**
Binds `line` of `lease` to the calling task, and unmasks it at the interrupt controller. From then on, each time the line fires, the kernel masks it and sets signal bit `sigbit` in the task. `AckWaitIrq` unmasks it again. An MSI (an LPI from `AllocMsi`) is edge-triggered: the kernel only sets the signal, and moves the LPI to the binding task's CPU.

The line stays bound until the task ends or its process dies. No call unbinds it.

A kernel call.

**INPUTS**
- `lease` -- the lease's handle, granted to the calling process.
- `line` -- the line's index in the lease, 0 to `li_Irqs` - 1.
- `sigbit` -- the signal bit, 16–63.

**RESULT**
0, or:
- `ERR_BADARG` -- `sigbit` is out of range, or the kernel has no interrupt controller to attach the line to.
- `ERR_BADHANDLE` -- `lease` isn't granted to the calling process, or `line` is out of range.
- `ERR_EXISTS` -- the line is already bound, by this task or another.

**NOTES**
- Only the grantee binds. The owner gets `ERR_BADHANDLE`, even while the lease is idle.
- Lines are attached at interrupt priority 3, the level for drivers' lines (`kernel/lease.c`).
- When the task ends, its lines are unbound, and another task of the process may bind them.
- The kernel doesn't check that `sigbit` was allocated with `AllocSignal`.

**BUGS**

**SEE ALSO**
[AckWaitIrq](#leaseackwaitirq), [CreateLease](#leasecreatelease)

---

## lease/CreateLease

**NAME**
CreateLease -- make a lease over a device

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t CreateLease(Handle root, uint64_t base, uint64_t size, const uint32_t *irqs,
                    uint32_t nirqs);
```

**FUNCTION**
Makes a lease over `size` bytes of registers at physical address `base`, and over the interrupt lines `irqs[0]` to `irqs[nirqs - 1]`. Line `i` of the lease is `irqs[i]`. The calling process owns the lease, which starts idle.

A kernel call.

**INPUTS**
- `root` -- the device root handle, owned by the calling process.
- `base` -- the registers' physical address, page-aligned.
- `size` -- whole pages, more than 0 and at most 1 GiB. The range must end within user addresses.
- `irqs` -- the interrupt controller's source numbers, none twice: 1–1023 for wired lines, or LPIs (from 8192) that `AllocMsi` gave. The kernel takes 1–9215 (`IRQ_SOURCES` in `kernel/irq.h`). May be `nullptr` when `nirqs` is 0.
- `nirqs` -- 0 to `LEASE_IRQS` (32: eight virtio-mmio slots share a page, and so a lease).

**RESULT**
The lease's handle (positive), or:
- `ERR_BADHANDLE` -- `root` isn't a device root the calling process owns.
- `ERR_BADARG` -- `size` is 0 or over 1 GiB; `base` or `size` isn't page-aligned; the range wraps or passes the top of user addresses; `nirqs` is over 32; or a line is 0, 9216 or more, or given twice.
- `ERR_BADADDR` -- `irqs` isn't readable.
- `ERR_RIGHTS` -- the range overlaps RAM, reserved memory, or registers the kernel drives itself (the console, the interrupt controller and its ITS, the watchdogs: `kernel/devmem.c`); or a line has a handler in the kernel. One range of memory is allowed: the GOP framebuffer, exactly, page-rounded (`fi_FbBase`, from `MapFirmware`).
- `ERR_EXISTS` -- the range or a line overlaps another lease's.
- `ERR_NOMEM` -- out of memory, or the system's handle table is full.
- `ERR_LIMIT` -- the calling process already holds 256 handles.

**EXAMPLE**
From `conformance/leaser.c`:
```c
int64_t l = CreateLease(root, dev, 4096, &irq, 1);

if (l < 0)
    return l;
```

**NOTES**
- The kernel checks the range only against what it knows: memory, its own devices and other leases. It doesn't check that a device is there.
- A framebuffer the display leases is memory, mapped as device memory: uncached, so the controller scanning it out sees each write.
- A lease lives while its owner's handle, its grant, or any of its DMA Areas needs it.

**BUGS**
- The IOMMU's registers aren't refused by the kernel. hwdb doesn't lease them (it reads each SMMU's state once at start, and refuses them to acpid), but the kernel would let the root's holder.

**SEE ALSO**
[GrantLease](#leasegrantlease), [QueryLease](#leasequerylease), [CloseHandle](exec.md#execclosehandle), [MapFirmware](#leasemapfirmware)

---

## lease/GrantConsole

**NAME**
GrantConsole -- give a process the serial console's input

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t GrantConsole(Handle root, Handle process);
```

**FUNCTION**
Makes `process` the one that may read what's typed at the kernel's serial console, with `ReadConsole`. It replaces any process given it before. hwdb gives it to the Shell it starts ([DOS ch. 2](../dos/02-shell.md)).

The kernel keeps writing its log to the same console. Only the input is given.

A kernel call.

**INPUTS**
- `root` -- the device root handle, owned by the calling process.
- `process` -- a process handle the calling process owns, held or running; or 0, the calling process itself.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `root` isn't a device root the calling process owns, or `process` isn't a process handle it owns.

**NOTES**
- The grant ends when the process dies (`lease_process_died` in `kernel/lease.c`).
- Give it before `StartProcess` for a program that reads at once.

**BUGS**
None known.

**SEE ALSO**
[ReadConsole](#leasereadconsole)

---

## lease/GrantFirmware

**NAME**
GrantFirmware -- map the firmware's description into a child

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t GrantFirmware(Handle root, Handle process);
```

**FUNCTION**
Maps the same tables `MapFirmware` maps, read-only and at their physical addresses, into `process`, a process the caller created. hwdb grants them to acpid, which loads the AML in them with ACPICA ([V4 step 4](../../docs/v4-plan.md)).

The child isn't told where they are. Its creator passes the address, which it learned from `MapFirmware`.

A kernel call.

**INPUTS**
- `root` -- the device root handle, owned by the calling process.
- `process` -- a process handle the calling process owns, held or running.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `root` isn't a device root the calling process owns, or `process` isn't a process handle it owns.
- `ERR_NOTFOUND` -- the process is dying or dead.
- `ERR_NOMEM` -- a page couldn't be mapped.

**NOTES**
- The mappings last until the child dies. There's no call to take them back.
- `conformance/leaser.c` starts `conformance/fwreader.c` twice: without the grant it faults reading the tables, and with it it reads them.

**BUGS**
After `ERR_NOMEM`, whatever was mapped before the failure stays mapped.

**SEE ALSO**
[MapFirmware](#leasemapfirmware), [GrantLease](#leasegrantlease)

---

## lease/GrantLease

**NAME**
GrantLease -- give a lease to a driver

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t GrantLease(Handle lease, Handle process);
```

**FUNCTION**
Grants an idle lease to `process`, a process the caller created. From then on that process may call `MapLease`, `BindIrq`, `AckWaitIrq`, `AllocDmaArea` and `QueryLease` on it. If the owner had mapped the registers, its mapping is removed first.

Grant it before `StartProcess`, to a process made with `PROCF_HELD`, and the driver has its lease from its first instruction. A running process may be granted one too.

A kernel call.

**INPUTS**
- `lease` -- a lease the calling process owns.
- `process` -- a process handle the calling process owns.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `process` isn't a process handle the calling process owns, or `lease` isn't a lease it owns.
- `ERR_BUSY` -- the lease is granted or stopping.
- `ERR_NOTFOUND` -- the process is dying or dead.

**NOTES**
- The grant lasts until the grantee dies. No call takes it back from a live process.
- A process may hold several leases. A lease is held by one process.
- Pass the driver the lease's handle value. It doesn't own the handle, but names the lease by it.
- The owner's mapping goes without warning. Touching a pointer it kept into the registers faults, and ends the owner.

**BUGS**
None known.

**SEE ALSO**
[CreateLease](#leasecreatelease), [RevokeLease](#leaserevokelease), [CreateProcess](exec.md#execcreateprocess), [StartProcess](exec.md#execstartprocess)

---

## lease/MapFirmware

**NAME**
MapFirmware -- map the firmware's description of the machine

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t MapFirmware(Handle root, struct FirmwareInfo *info);
```

**FUNCTION**
Maps the firmware's tables into the calling process, read-only, at their physical addresses, and fills in `info`:
- On a device-tree machine, the DTB, at `fi_Dtb`, `fi_DtbSize` bytes long.
- Under ACPI, the RSDP, at `fi_Rsdp`, and the XSDT, every table the XSDT lists, the DSDT and the FACS. Pointers between them work as they are.

An address is 0 for whichever the kernel didn't read.

It also describes the framebuffer UEFI's GOP left set up, if there is one in a format Planar draws (GOP format 0, RGBX, or 1, BGRX, 8 bits a colour):

| Field | |
|---|---|
| `fi_FbBase`, `fi_FbSize` | Where it is and how big; base 0 if there's none |
| `fi_FbWidth`, `fi_FbHeight` | Pixels |
| `fi_FbStride` | Pixels a line |
| `fi_FbFormat` | 0 or 1 |

The framebuffer isn't mapped. It's the one range of memory `CreateLease` will cover. hwdb gives it a `simple-framebuffer` node of its own, for the display driver ([Devices ch. 5](../devices/05-drivers.md#display)).

A kernel call.

**INPUTS**
- `root` -- the device root handle, owned by the calling process.
- `info` -- where to store the addresses.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `root` isn't a device root the calling process owns.
- `ERR_NOMEM` -- a page couldn't be mapped.
- `ERR_BADADDR` -- `info` isn't writable.

**NOTES**
- The tables are mapped where they lie, not as an Area: `QueryArea` doesn't find them and they can't be shared.
- Whole pages are mapped, so the bytes next to a table on its pages can be read too.
- Calling it again maps the same pages again, which does no harm. The mappings last until the process dies.

**BUGS**
After `ERR_NOMEM` or `ERR_BADADDR`, whatever was mapped before the failure stays mapped.

**SEE ALSO**
[CreateLease](#leasecreatelease), [GrantFirmware](#leasegrantfirmware)

---

## lease/MapLease

**NAME**
MapLease -- map a lease's registers

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t MapLease(Handle lease, void **address);
```

**FUNCTION**
Maps the lease's registers into the calling process, read-write, as device memory (Device-nGnRE on AArch64), at their physical address, and stores that address in `*address`.

The grantee may map a granted lease. The owner may map a lease that is idle or stopping, to stop the device after its driver has died.

A kernel call.

**INPUTS**
- `lease` -- the lease's handle.
- `address` -- where to store the registers' address.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- the lease is granted and the caller isn't its grantee, or it isn't granted and the caller isn't its owner.
- `ERR_EXISTS` -- the registers are mapped already, by this process or the other.
- `ERR_NOMEM` -- the pages couldn't be mapped.
- `ERR_BADADDR` -- `address` isn't writable. The registers are mapped all the same.

**NOTES**
- One process maps the registers at a time. The owner's mapping goes at `GrantLease` or when it closes the lease. The grantee's goes when it dies. No call unmaps them.
- `*address` is `li_Base`, the start of the lease's first page. A device that starts mid-page is at an offset from it.
- The mapping isn't an Area: `QueryArea` doesn't find it.
- On RISC-V the platform's PMAs make device ranges uncached I/O; the page tables say nothing more.

**BUGS**
None known.

**SEE ALSO**
[GrantLease](#leasegrantlease), [QueryLease](#leasequerylease)

---

## lease/QueryLease

**NAME**
QueryLease -- describe a lease

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t QueryLease(Handle lease, struct LeaseInfo *info);
```

**FUNCTION**
Fills in `info` for a lease the calling process owns or holds:

| Field | |
|---|---|
| `li_Base`, `li_Size` | The registers' physical address, which is where they're mapped, and size |
| `li_State` | `LEASES_IDLE`, `LEASES_GRANTED` or `LEASES_STOPPING` |
| `li_Irqs` | How many lines it has |
| `li_DmaPages` | Pages in its live DMA Areas |
| `li_QuarantinedPages` | Pages waiting for `RevokeLease` |
| `li_LentPages` | Pages of others' Areas its dead grantee mapped, held as well until `RevokeLease` |

A kernel call.

**INPUTS**
- `lease` -- a lease the calling process owns or is granted.
- `info` -- where to store the description.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- the calling process neither owns `lease` nor is granted it.
- `ERR_BADADDR` -- `info` isn't writable.

**NOTES**
- The lines' source numbers aren't reported, only how many there are.
- After a driver's death, `li_QuarantinedPages` says how much of its DMA memory the device may still write to.

**BUGS**
None known.

**SEE ALSO**
[CreateLease](#leasecreatelease), [RevokeLease](#leaserevokelease)

---

## lease/ReadConsole

**NAME**
ReadConsole -- read what's been typed at the serial console

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t ReadConsole(char *buf, uint32_t len);
```

**FUNCTION**
Copies to `buf` up to `len` bytes typed at the serial console, at most 64, as many as are waiting. It doesn't wait: with nothing typed it returns 0.

A kernel call.

**INPUTS**
- `buf` -- where to put them.
- `len` -- bytes wanted; more than 64 is taken as 64.

**RESULT**
The bytes read (0 or more), or:
- `ERR_RIGHTS` -- the calling process wasn't given the console (`GrantConsole`).
- `ERR_BADADDR` -- `buf` isn't writable. The bytes read are lost.

**EXAMPLE**
The Shell's loop, from `cmd/shell.c`:
```c
char got[64];
int64_t r = ReadConsole(got, sizeof got);

if (r == 0) {
    nap(10);            /* nothing typed: look again in 10 ms */
    continue;
}
```

**NOTES**
- The console's receive FIFO is read directly (`console_getc` in `kernel/console.c`). There's no interrupt or buffer: what isn't read before the UART's FIFO fills is lost. Poll often.

**BUGS**
No receive interrupt, so a reader must poll.

**SEE ALSO**
[GrantConsole](#leasegrantconsole)

---

## lease/RevokeLease

**NAME**
RevokeLease -- take back a lease whose driver has died

**SYNOPSIS**
```c
#include <planar/lease.h>
int64_t RevokeLease(Handle lease);
```

**FUNCTION**
Frees a stopping lease's quarantined DMA Areas, lets go of the lent Areas it held, and makes it idle, ready to grant again. The owner calls it once it has stopped the device. On an idle lease it does nothing and returns 0.

A kernel call.

**INPUTS**
- `lease` -- a lease the calling process owns.

**RESULT**
0, or:
- `ERR_BADHANDLE` -- `lease` isn't a lease the calling process owns.
- `ERR_BUSY` -- the lease is granted. End the grantee first.

**EXAMPLE**
A driver killed with DMA outstanding, from `conformance/leaser.c`:
```c
KillProcess((Handle)d);
Wait(1ULL << 21);                       /* dead: the lease is stopping */
MapLease((Handle)l, (void **)&regs);
regs[(off + 0x70) / 4] = 0;             /* reset the virtio device */
while (regs[(off + 0x70) / 4] != 0)
    ;
RevokeLease((Handle)l);
```

**NOTES**
- The kernel can't tell whether the device has stopped. It takes the call as the owner's word. Revoke a lease whose device still runs, and the device may write to memory that has been given to someone else.
- The owner's mapping of the registers stays.
- A DMA Area still mapped by a process the driver shared it with isn't freed. `li_DmaPages` still counts it.
- A held Area that nobody maps or names any more is freed here; one still in use stays its user's.
- If the owner dies while the lease is stopping, its quarantined and held Areas stay allocated for good, and the kernel log says so. (`CloseHandle` on a stopping lease gives `ERR_BUSY`.)

**BUGS**
None known.

**SEE ALSO**
[AllocDmaArea](#leaseallocdmaarea), [GrantLease](#leasegrantlease), [KillProcess](exec.md#execkillprocess)
