# The input class

*Checked against V5; its clients against V7 step 5.*

## What it's for

Keys and the mouse, from every keyboard and mouse there is, to one program: the Shell, or `intuition` while it runs (V7 step 4, [Libraries ch. 13](../libraries/13-intuition.md)). intuition starts only when it's wanted, so the console Shell keeps the keys otherwise (v7-plan decision 4). Whichever asks second goes without: intuition then runs with no pointer or keys, and says so. `input` (`servers/input`) is the class's server: one for the system, with no device of its own. The drivers that find keyboards and mice send it what they report, and it turns that into **events** on a ring the client reads with no message per event. USB's are the only ones so far: `xhci` (`drv/xhci`) drives the USB host controllers, walks their hubs, and reads every keyboard and mouse that speaks the HID boot protocol.

The serial console stays as it was ([chapter 3](03-console.md)): the Shell reads both.

## Concepts

- **One port, two sides.** `input` publishes "input", with `struct InputMsg` messages (`planar/devices/input.h`). A client sends `INPUT_OPEN` and `INPUT_INFO`. A driver sends `INPUT_ATTACH` for each device it finds, `INPUT_REPORT` for each report, raw, and `INPUT_DETACH` when it goes.
- **The ring** (`struct InputRing`) is one page, mapped read-only into the client that took it with `INPUT_OPEN`. `ir_Head` counts every event ever written; event *n* is at `ir_Event[n % INPUT_RING]`. A client keeps its own count, reads up to `ir_Head`, and sleeps in `WaitAddr` on it: `input` wakes it with `WakeAddr` when it moves. A client more than `INPUT_RING` (127) behind has lost the oldest; so has one whose event was overwritten while it copied it, which it sees by reading `ir_Head` again after.
- **One client at a time.** The ring is the first opener's until its connection goes, dead or not; another meanwhile gets `ERR_BUSY`. A restarted `input` has a new ring, at a new address.
- **Events** (`struct InputEvent`, 32 bytes) have a class:
  - `IECLASS_KEY`: `ie_Code` is the key's usage on the HID keyboard page (USB HID Usage Tables §10: `IEKEY_A` is 4), and `ie_Flags` has `IEF_UP` when it's let go. A key going down has `ie_Char`, what it types on a US layout, or 0 (arrows, F-keys). Enter types `'\n'`, Backspace 8, Delete 127, and Ctrl with a letter its control character.
  - `IECLASS_BUTTON`: `ie_Code` 0 is the left button, 1 the right, 2 the middle.
  - `IECLASS_MOVE`: the mouse moved `ie_X`, `ie_Y`, down being +y.
  Each carries `ie_Qualifier`, the modifiers held then (a boot report's first byte: `IEQ_LSHIFT`, `IEQ_RCTRL` and so on) and `IEQ_CAPSLOCK`, and `ie_Time`, as `CurrentTime` had it when `input` wrote it. The modifiers come as keys too (`0xe0` to `0xe7`).
- **Repeat** is `input`'s own: a key that types something, held 500 ms, repeats every 33 ms, each with `IEF_REPEAT`, until it's let go or another goes down. Boot keyboards don't repeat by themselves.
- **When a driver goes**, dead or not, its devices go with it, and every key they held is let go: the client sees each one come up. When it comes back it attaches them again.
- **Caps Lock** is kept by `input`, not the keyboard: its light isn't set.

## Using it

From `conformance/inputtest.c`, shortened:
```c
struct MsgPort mp;
struct InputMsg *m;
const struct InputRing *ring;
uint64_t tail, head;
struct InputEvent e;

ConnectPort(&mp, "input", 20, 4, 2 * sizeof *m);
m = AllocMsg(&mp, sizeof *m);
memset(m, 0, sizeof *m);
m->im_Msg.mn_Length = sizeof *m;
m->im_Op = INPUT_OPEN;
m->im_Task = FindTask(nullptr);         /* where to map the ring */
DoMsg(&mp, &m->im_Msg);                 /* im_Ring, or ERR_BUSY */
ring = (const struct InputRing *)m->im_Ring;
tail = ring->ir_Head;                   /* from now on */

for (;;) {
    head = __atomic_load_n(&ring->ir_Head, __ATOMIC_ACQUIRE);
    if (head - tail > INPUT_RING)
        tail = head - INPUT_RING;       /* lost some */
    if (tail == head) {
        WaitAddr(&ring->ir_Head, head, 0);
        continue;
    }
    e = ring->ir_Event[tail % INPUT_RING];
    if (__atomic_load_n(&ring->ir_Head, __ATOMIC_ACQUIRE) - tail > INPUT_RING)
        continue;                       /* overwritten as it was copied */
    tail++;
    if (e.ie_Class == IECLASS_KEY && !(e.ie_Flags & IEF_UP) && e.ie_Char)
        /* ... typed e.ie_Char ... */;
}
```

A client that should notice `input` going looks at `PortGone(&mp)` now and then (with a deadline on `WaitAddr`), and connects again: the Shell tries every half second. intuition is told at once, by its connection's signal. It lets go of the ring, waits for input's port with a task of its own, and opens the ring again, the pointer where it was (V7 step 5). The keyboards and mice come back a little after, as xhci tells the new input of them: what's sent before is lost. A client that held the ring and died is let go of when input sees its connection go, so the next one may find it busy for a moment: intuition, starting, tries again for a second.

intuition reads the ring with a task of its own waiting in `WaitAddr` on `ir_Head`, which signals intuition's main task. Mouse moves are clamped to the screen, with no acceleration.

## Rules

- The ring is read-only: a client keeps its own count.
- Copy an event out before using it, and check `ir_Head` after: the slot may have been written again meanwhile.
- `im_Task` must be the client's own task, for `INPUT_OPEN`: the server maps the ring into it.
- A driver's `im_Device` numbers are its own: two drivers may use the same ones.

## The drivers

**xhci** (`drv/xhci`, xHCI 1.2). One process for every controller hwdb gives it:
- QEMU's `qemu-xhci`, found by `pci` by its class (`0c0330`), interrupting by MSI-X vector 0, or polled every 4 ms where there's no MSI (RISC-V's PCI);
- the CIX's ten Cadence controllers, which UEFI leaves in host mode: hwdb gives xhci their `CIXH2031` descriptions, on their xHCI windows and host lines alone (`servers/hwdb/quirks.c`);
- the K1's DWC3, behind the RV2's USB-A ports. Its clock and reset are `power`'s, voted for by hwdb. xhci votes for `power`'s set `"usb"` itself: the combo PHY's mode and reset, and the hub's VBUS. Then it starts both PHYs and makes the core a host (`dwc3.c`).

Bind resets each controller, takes it from the firmware (USB Legacy Support), and starts it with its rings in DMA Areas below 4 GiB, uncached: neither board's controllers snoop. After, from the loop, each port with something on it is reset and its device addressed, its descriptors read and its first configuration set. A **hub** has its ports powered and looked at every 250 ms; a full- or low-speed device behind a high-speed hub is reached through that hub's transaction translator (single TT). A **HID interface** of the boot subclass, keyboard or mouse, is put in the boot protocol and its interrupt endpoint read for ever, each report sent to `input` as it comes. The controller interrupts at most every 32 ticks of its moderation counter (`IMOD`): 8 µs in xHCI's 250 ns ticks, 1 ms on the K1's DWC3, whose ticks are 32 kHz. Eight transfers stay queued on it, so the controller goes on polling while the driver is late: a keyboard reports only changes, and a key down and up between two polls would be lost. Only USB 2's ports are walked: keyboards and mice are never SuperSpeed, and a USB 3 hub has a USB 2 hub inside it.

A dead xhci's controllers are halted by hwdb (USBCMD.R/S cleared, until HCH), before its DMA Areas leave quarantine.

## The machines

**QEMU**: an xHCI on PCI, with a full-speed hub, and a keyboard and a mouse behind it (`QEMU_USB` in the Makefile). The boot check types what the input test asks for through QMP (`tools/boot-check.py`).

**The CIX**: all ten controllers. The rig's keyboard and mouse are on `USB1`, behind a single-TT high-speed hub, at full speed. Which other connector is which controller isn't known yet.

**The RV2**: the onboard Genesys USB 3 hub on the DWC3. Its USB 2 half has the USB-A ports, whose VBUS is GPIO 123. A low-speed keyboard and mouse work through its TT. Another USB-A port is the OTG controller's, an EHCI with VBUS on GPIO 126, which nothing drives yet.

## Functions used

- [ConnectPort](../autodocs/msg.md#execconnectport), [DoMsg](../autodocs/msg.md#execdomsg), [WaitAddr](../autodocs/exec.md#execwaitaddr), [PortGone](../autodocs/msg.md#execportgone): the client's side.
- [AllocArea](../autodocs/exec.md#execallocarea), [ShareArea](../autodocs/exec.md#execsharearea), [UnshareArea](../autodocs/exec.md#execunsharearea), [WakeAddr](../autodocs/exec.md#execwakeaddr): the server's.
- [AllocDmaArea](../autodocs/lease.md#leaseallocdmaarea): xhci's rings, contexts and buffers.
