# PCI: a bus driver that describes what it finds

*Checked against V5.*

## What it's for

PCI devices aren't in the firmware's description: the bus has to be walked. The `pci` driver (`drv/pci`) binds to each PCI host bridge and tells hwdb what's behind it, and hwdb then matches, leases and starts drivers for those functions like any other device. A **bus driver** is an hwdb source, as `acpid` is (§11.6). This chapter is for readers of `drv/pci` and `servers/hwdb/pci.c`, and for anyone writing another bus driver.

## Concepts

A host bridge is a `pci-host-ecam-generic` node (device tree) or a PNP0A08 device (ACPI, with the MCFG's ECAM). Its lease is its ECAM: config space, 1 MiB a bus.

While it binds, `pci` sends hwdb two kinds of message on its connection (`drv_hwdb_msg`, `drv_hwdb`):
- **`HWDB_PCI_HOST`**: what the bridge decodes. hwdb answers `struct PciHost`: its segment and buses, its memory windows (`PCI_MEM`, `PCI_64`, `PCI_PREFETCH`), and whether its functions' MSIs reach an ITS (`ph_Msi`: it has an `msi-map` or IORT mapping).
- **`HWDB_PCI_FUNCTION`**: one function, `struct PciFunction`: requester ID, vendor and device IDs, class, its memory BARs, and its MSI-X table.

hwdb adds a node per function under the bridge, named "BB:DD.F", with keys most specific first, after the PCI binding to Open Firmware:

| Key | Example | Matched by |
|---|---|---|
| `pciVVVV,DDDD` | `pci1b36,0010` | A driver for that device |
| `pciclass,CCSSPP` | `pciclass,010802` | A class driver: NVMe |
| `pciclass,CCSS` | `pciclass,0805` | |

The function's BARs are its registers, and it waits on the bridge (its parent) and on the bridge's IOMMU. hwdb checks what `pci` says: the function must be on the bridge's buses, and each BAR inside one of its memory windows, or it's refused.

**MSI-X.** For a function with MSI-X behind a bridge whose MSIs reach an ITS, hwdb asks the kernel for LPIs for its first 4 vectors (`AllocMsi`), by the device ID the bridge's map gives its requester ID. They go in its lease as lines. Its driver gets `dd_MsixTable`, `dd_MsiDoorbell` and `dd_NLines`, writes each vector's entry (the doorbell, and the event number *i*) and unmasks it. `pci` has already enabled MSI-X in config space.

## What pci does on the bus

- It walks from the bridge's first bus down through each bridge the firmware numbered. Below a PCIe root port or switch downstream port it scans only device 0: the link has one device, and others may answer as aliases of it.
- It sizes each memory BAR. One the firmware left at 0, or outside every window (QEMU booted with `-kernel`), gets an address in a window, aligned to its size and clear of the others.
- It turns on memory decoding and bus mastering, and MSI-X when there's an ITS.

The firmware trains the links and numbers the buses; `pci` doesn't.

## Rules for a bus driver

- Describe; don't drive. The functions' drivers are hwdb's to start.
- Send each child as you find it, during Bind. A child sent again (a restart) is the node hwdb already has.
- Everything you send is checked: hwdb trusts a bus driver no more than any other.

## Planned

- Stopping a function by clearing Bus Master Enable and FLR (§11.4). Today a function's driver's stop method is its own (NVMe's `CC.EN`).
- Bridges the firmware didn't number, and I/O BARs.

## Functions used

[drv_hwdb](../autodocs/drv.md#drvdrv_hwdb), [AllocMsi](../autodocs/lease.md#leaseallocmsi)
