# Drivers: hwdb, leases, drv.library, supervision

*Checked against V5.*

## What it's for

A driver is a user-mode process that owns one or more devices for as long as it runs. This chapter says how it gets them, what library it is built on, and what happens when it dies. Read it before writing a driver; a program that only uses devices needs [chapter 2](02-block.md) or [chapter 3](03-console.md).

## Concepts

**hwdb** is the first server. The kernel starts it with the **device root**, the handle that may create leases ([lease](../autodocs/lease.md)). hwdb:
- reads the firmware's description (`MapFirmware`): the device tree, or ACPI's tables and, through `acpid`, the namespace;
- builds one graph of devices, applies the board's quirks (`servers/hwdb/quirks.c`), and matches each device against the drivers' tables (`servers/hwdb/drivers.c`): by `compatible`, `_HID`/`_CID`, or a PCI function's keys;
- starts each driver once every device it needs is bound: its bus, interrupt controller, clocks and the like. The order comes from the graph's references.

A **unit** is one driver process and what it's leased. Devices whose registers share a page go in one lease, served by one process: QEMU's eight virtio-mmio slots in a page are one unit.

A **lease** is the unit's registers and interrupt lines. hwdb creates it, grants it to the driver's process, and starts the process. The driver maps the registers at their physical address and allocates its DMA memory against the lease. The driver doesn't own the lease: hwdb does.

A **class port** is how programs reach a device: a port named for the class and the device's unit, "nvme.0", "sd.0", "con.0". The unit is fixed when hwdb matches the device, so a restarted driver publishes the same name.

## The driver's life

1. hwdb creates the process held, with a token as its argument, grants it its leases, and starts it.
2. The driver connects to "hwdb" and asks for its devices with the token (`HWDB_BIND`). The answer, `struct HwdbMsg`, lists its leases (`hm_Range`) and each device (`hm_Device`): registers, the index of its interrupt line in the first lease, its unit, the key it matched, and for a PCI function its MSI-X table and vectors ([chapter 4](04-pci.md)).
3. It binds each device or declines it, publishes the class ports of those it took, and says so (`HWDB_STARTED`).
4. It serves clients until it dies or hwdb goes.

`drv.library` does steps 2 to 4. A driver gives `drv_main` its callbacks:

| Callback | When |
|---|---|
| `bind` | Each device: `DRV_OK` takes it, `DRV_DECLINE` passes (an empty slot, a card that isn't there), a negative error ends the process |
| `start` | Each device taken: publish its port with `drv_publish` |
| `interrupt` | A device's line, or one of its MSI-X lines, fired |
| `request` | A client's `struct DrvRequest`: up to 64 bytes in `dr_Data`, `DRV_READ` or `DRV_WRITE`; answered when it returns |
| `message` | A class's own messages, in place of `request`: answered with `drv_reply`, now or later |
| `poll` | Each time round the loop, for a device without interrupts; true keeps the loop from sleeping |

## Using it

The smallest real driver, `drv/rtc-pl031/main.c`, reads a register on each request:

```c
static void request(struct drv *, struct drv_dev *dev, struct DrvRequest *r)
{
    if (r->dr_Op != DRV_READ || r->dr_Len < 8) {
        r->dr_Msg.mn_Error = (int32_t)ERR_BADARG;
        return;
    }
    ...
    r->dr_Len = 8;
}
```

A driver with DMA allocates it in `bind` against the lease, so it is quarantined if the driver dies:

```c
if (AllocDmaArea(drv_lease(d), PAGE, 0, &at) < 0)
    return -10;
```

A class that moves real data uses `message`, so requests can be outstanding at once and answered as they finish ([chapter 2](02-block.md)).

## When a driver dies

hwdb sees every death. The teardown is the kernel's and hwdb's, never the dead driver's:
1. The kernel masks the driver's level-triggered lines at once, quarantines the DMA Areas it didn't free, and holds the memory clients lent it, their connection arenas (`li_LentPages`).
2. hwdb runs the unit's **stop method**, which makes the device stop touching memory: virtio's status register to 0, NVMe's `CC.EN` to 0, SDHCI's Software Reset For All, a GPU's soft reset, every display plane off but the console's ([chapter 5](05-drivers.md)). A unit with no stop method is safe only if it kept no DMA; one that did stays quarantined and isn't restarted.
3. hwdb calls `RevokeLease`, which frees the quarantine and the held memory, and starts the driver again.

**Power before the lease** ([chapter 8](08-power.md)). A driver whose device needs power has it voted on by hwdb before its lease is granted, so a driver never touches a device that's off. hwdb, not the driver, holds the vote, and lets it go only after the stop method has run: a stop method must touch a powered device, and on the CIX an access to an unpowered GPU wedges the board.

**Supervision** (`servers/hwdb/server.c`): a death hwdb asked for is restarted at once. Any other is restarted after 10 ms, then twice as long each time, and given up on at 5 failures within 10 s. A driver that hasn't said it started within 2 s has failed.

Clients see a death as `ERR_PORT_DIED` on their connection. A class's library reconnects once the port is back ([chapter 2](02-block.md#restarts)).

**The kill test** (hwdb's test mode, `servers/hwdb/killtest.c`) kills every driver with a client, a dozen times each, some while binding, most under I/O, and checks each teardown and that every client recovered. Then V5's phases, one class each: `power` killed and every driver voted for again (`power.c`); a display client killed, and display killed under its planes (`display.c`); `xhci` and `input` killed under an input client (`input.c`); a GPU client killed with jobs in flight, and the GPU server killed under one (`gpu.c`). Last, the load phase: one GPU client runs jobs without a pause while display, input, power, another GPU client and the GPU server are killed around it (`gpu.c`, `T_LOAD`).

## Rules

- Bind does all the setup that can fail. After `HWDB_STARTED` a driver only serves.
- Reset the device in `bind`: a dead driver's restart finds it as the last one left it.
- Allocate DMA with `AllocDmaArea`, never `AllocArea`: only DMA Areas are quarantined.
- Hand a device only memory that's yours (a DMA Area) or a client's buffer in its connection's heap: check it with `drv_in_heap`.
- Read a client's message once, into your own memory, then check it. The client can change it while you look.
- Never `DoMsg` a client, and never wait on hwdb after start. Calls go down the graph; notices come up (§11.6).

## Differences from Exec

- A device isn't opened with `OpenDevice`: a program connects to its class port.
- A driver is a process, not a task in the caller's memory, and dies alone.

## Functions used

[drv_main](../autodocs/drv.md#drvdrv_main), [drv_publish](../autodocs/drv.md#drvdrv_publish), [drv_reply](../autodocs/drv.md#drvdrv_reply), [drv_in_heap](../autodocs/drv.md#drvdrv_in_heap), [drv_lease](../autodocs/drv.md#drvdrv_lease), [AllocDmaArea](../autodocs/lease.md#leaseallocdmaarea), [BindIrq](../autodocs/lease.md#leasebindirq), [RevokeLease](../autodocs/lease.md#leaserevokelease)
