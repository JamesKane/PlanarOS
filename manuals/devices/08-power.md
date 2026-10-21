# The power class

*Checked against V5 step 2; the K1's `"usb"` set against step 4.*

## What it's for

Clocks, resets and power domains. A device that is off, unclocked or held in reset doesn't answer its registers, and on some machines reading one then takes the machine down: the CIX's GPU raises an SError, and the board needs a power cycle. So nothing drives one by hand. The `power` server (`drv/power`) owns the controllers, and hwdb asks it to turn each driver's devices on before it grants their leases ([docs §11.5](../../docs/11-driver-model.md), D3). A driver that holds a lease can rely on its device being powered.

It drives:
- **the K1** (the RV2): its seven power domains, and the clock gates and resets in its APMU of the devices V5 uses: SD, the display and HDMI, USB3, and the GPU;
- **the CIX P1**: SCMI, clocks through mailbox 6 to the system control processor, and power domains through TF-A by SMC;
- **QEMU's model**, which hwdb's test mode adds, as there's nothing to power there.

## Concepts

- **One port, "power".** A client connects and sends `struct PowerMsg` messages (`planar/devices/power.h`).
- **A vote.** `POWER_ON` names a set and a tag of the client's own choosing. The server turns on what in the set is off and keeps it on while the vote stands. `POWER_OFF` with the tag takes it back. A tag is used once at a time (`ERR_EXISTS`).
- **A set** is either up to 24 firmware references, or a name. Each reference (`struct PowerRef`) is a kind (`POWER_DOMAIN`, `POWER_CLOCK` or `POWER_RESET`), which of power's devices provides it (in the order hwdb gave power them), and the specifier's cells as the firmware wrote them. A name is a set the server knows for a device the firmware doesn't describe well enough: `"gpu"` on every machine, and `"usb"` on the K1 (below). A set may hold a fourth kind, `POWER_SUPPLY`, which no reference names yet.
- **Order.** On, a vote turns domains on first, then clocks, then releases resets. Off goes the other way round.
- **All or nothing.** If one resource of a vote fails (a domain that doesn't say it's on in time: `ERR_TIMEOUT`), the ones the vote turned on go off again, and the vote isn't held.
- **Shared.** A resource is on while any vote holds it. It goes off when its last vote goes.
- **The firmware's are kept.** What's on when the server starts is never turned off: something nobody votes for may need it (the console's clock, the bus the display is on). U-Boot's SD and display clocks on the RV2 are kept, so killing the SD driver doesn't stop its clock.
- **Votes are the connection's.** When a client goes, dead or not, its votes go with it.
- **Left alone.** A reference power doesn't drive (a clock outside the registers it's leased) is left as the firmware set it, and counted in `pm_Untouched`. It isn't an error.

`POWER_QUERY` reads a set from the hardware: `pm_Total` resources, `pm_On` of them on, `pm_Kept` of them the firmware's.

## Using it

hwdb is power's first client, and makes every driver's vote: a driver doesn't vote for its own device. A server that powers something no firmware reference describes votes by name, as the GPU servers will (steps 5 and 6). From `conformance/powervote.c`:
```c
struct MsgPort mp;
struct PowerMsg *m;

ConnectPort(&mp, "power", 20, 4, 2 * sizeof *m);
m = AllocMsg(&mp, sizeof *m);
memset(m, 0, sizeof *m);
m->pm_Msg.mn_Length = sizeof *m;
m->pm_Op = POWER_ON;
m->pm_Tag = 1;
strcpy(m->pm_Name, "gpu");          /* or pm_Count references in pm_Ref */
DoMsg(&mp, &m->pm_Msg);
/* m->pm_Msg.mn_Error: 0, and the GPU is powered until we go */
```

## Rules

- Vote before touching a device you power yourself. A lease from hwdb comes powered.
- Don't vote to keep something on for longer than you use it: dying is enough to let go, and so is `POWER_OFF`.
- What the firmware left on stays on. Don't rely on a vote's end to turn a device off.
- If power restarts, every vote is gone. hwdb votes again for its drivers. A client of its own sees `ERR_PORT_DIED` and must connect and vote again.

## The machines

**The K1** (`drv/power/k1.c`, from the vendor kernel's `x1-pm_domain.c`, `ccu-ky-x1.c` and `reset-ky-x1.c`). U-Boot's tree has the clock controller, the reset controller and the power domains each list the same eight register blocks. Everything power drives is in one of them, the APMU at `0xd4282800`, and hwdb leases power that page alone; the rest stay as U-Boot set them. U-Boot's cell numbers: clocks are Linux's plus 41, resets are Linux's, domains are mainline's (VPU 0, GPU 1, LCD 2, ISP 3, audio 4, GNSS 5, HDMI 6). The display's, audio's and HDMI's domains are switched by the hardware, the rest by the vendor's software sequence: sleep switches, then isolation released, then the status bit within 60 ms. `"gpu"` is the GPU's domain, its clock (APMU `0xcc` bit 4) and its reset (bit 1). `"usb"`, which xhci votes for itself, is what U-Boot's tree leaves out of the DWC3's: the combo PHY's port A set to USB 3 (APMU `0x110` bit 3), its reset (`0x3cc`, released by setting bits 3 to 5 and clearing bit 8), and the onboard hub's power and its ports' VBUS, GPIO 123 driven high (U-Boot's tree says 79, and leaves both low). For that, hwdb leases power the GPIO controller's page with the APMU's: it is the GPIO's only owner (`servers/hwdb/quirks.c`).

**The CIX** (`drv/power/scmi.c`). Two SCMI channels in the SMT layout, each polled:
- clocks (protocol `0x14`) to the SCP, through SHM0, the first `0x80` bytes of mailbox 6's page at `0x6590000`, rung at `+0x80`. The rest of the page is the CPPC's and the PCC's, and power never touches it;
- power domains (protocol `0x11`) to TF-A, through the channel at `0x84380000`, rung by SMC `0xc2000001`, which the kernel makes for power's lease ([CallLease](../autodocs/lease.md#leasecalllease)).

IDs are the vendor tree's: ACPI names none, so only named sets reach them. `"gpu"` is domain 21 and the clocks `gpu_clk_core` (`0x1f`) and `gpu_clk_stacks` (`0x20`): the first two of the G720's five power-on steps. The rest are in the GPU's own registers (step 5).

**QEMU's model** (`drv/power/model.c`): four domains, two clocks and a reset, all bits in memory. Domain 0 starts on, and domain 3 never comes on, for the error path. hwdb's test mode has the RTC need domain 1, so the kill test's deaths take that vote away and its restarts bring it back.

## Functions used

- [ConnectPort](../autodocs/msg.md#execconnectport), [AllocMsg](../autodocs/msg.md#execallocmsg), [DoMsg](../autodocs/msg.md#execdomsg): the client's side.
- [SetLeaseCall](../autodocs/lease.md#leasesetleasecall), [CallLease](../autodocs/lease.md#leasecalllease): the CIX's SMC.
- [drv_main](../autodocs/drv.md#drvdrv_main)'s `gone`: votes per connection.
