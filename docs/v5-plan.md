# V5 plan: queue leases, native scanout, power and input

**Status: proposed (2026-10-20).** Scoped from the V4 exit and from three surveys of the reference trees: Linux's and FreeBSD's panthor, komeda and SCMI on the CIX, and the vendor kernel's `img-rogue` and `ky` drivers on the RV2 ([vendor-kernel branches](#sources)). The four scope decisions at the end are made.

**Goal (§8):** the first coprocessor in user space, and the hardware the Workbench phase (V6) needs under it. An app on the CIX submits GPU work and waits for it with no kernel or server in the data path (§5.1). The RV2 does the same through a server queue, with the same API. Both boards scan out natively from the display controller the firmware left running, take keyboard and mouse from USB, and power devices through a `power` server.

**What's already built.**
- Leases, lines, DMA Areas with quarantine, `CreateProcess`, timers and the kill test (V4 steps 1 and 3). hwdb starts drivers in graph order and restarts them.
- `display` draws a text console on the firmware's framebuffer, and the Shell reads the serial console (V4 step 7, decisions 1 and 2).
- `drv_dma_clean` and `drv_dma_flush` for devices that don't snoop (the K1's), and `AREAF_DMA32`.
- hwdb sees every device this plan drives: on the CIX `GPU_`/`GPUP` (CIXH5000/5001), `DPU0`–`DPU4` (CIXH5010), `SCMI` (CIXHA006) and `USB0`–`USB9` (PNP0D10); on the RV2 the GPU at `0xcac00000`, the DPU at `0xc0440000`, and the DWC3 at `0xc0a00000`. All are "no driver" today.

**Not built, and needed here** (all three built in step 1):
- `WaitAddr`/`WakeAddr` ([docs §4.1](04-ipc.md)): the sleeping wait on a sync object (§5.1), and on V6's `safe_seq`/`disp_seq` (§16.3 pillar 4).
- Mapping one page of a device to an app: a CSF doorbell (§5.1's `AREAF_DEVICE`). Today only hwdb makes leases, and only drivers hold them.
- Non-cacheable and write-combining Areas ([docs §3.3](03-memory.md)): the G720 doesn't see CPU writes that are still in cache (below).

**Exit (§8):**
- On the CIX, an app submits a CSF job to its own queue and waits on its sync object. Between the doorbell and the sync object's update there is no kernel call and no server message, counted with `QuerySystem`.
- On the RV2, the same app API runs a job on the BXE-2-32 through a server queue (decision 1).
- On both boards, `display` flips buffers at vblank on the mode the firmware left, with at least two planes and a cursor plane, and comes back after being killed without the screen going dark.
- On both boards, a USB keyboard drives the Shell, and mouse events reach an `input` client.
- `power` powers the GPU on the CIX and the K1's domains, and the kill test checks that a dead driver's votes go (moved from V4).
- A GPU client killed with work in flight leaves no queue running and no Area in use. The GPU server can be killed and restarted under load.
- The manuals are current (roadmap rule), with *Libraries* chapters on futexes and queue leases, and *Devices* chapters on power, display and input.

Each step ends with `make test-all` green and a `V5.n done` line in the transcript.

| Step | Content |
|---|---|
| **1. The kernel calls queue leases need** | `WaitAddr`/`WakeAddr`, sub-leases of one device page granted to an app, non-cacheable and write-combining Areas, and a device's address window (the K1's `dram_range1`). |
| **2. `power`** | Native SCMI on the CIX (mailbox and SMC), the K1's PMU domains and CCU clocks and resets, votes per connection, power before lease (§11.5). |
| **3. Native scanout** | `display` takes over the firmware's mode: the CIX's DPU4 (a komeda D71 subset) and the K1's saturn DPU. Flips at vblank, planes, a cursor plane, restart without a blank. |
| **4. USB input** | xHCI with hubs, HID boot keyboard and mouse, the K1's DWC3 and PHY, and an `input` server. The Shell takes its keys from USB. |
| **5. `gpu-panthor` on the CIX** | The G720's power-on, firmware, MCU boot and global interface. One pinned group per lease, with its ring, interfaces and doorbell mapped to the app. A CSF job and its sync object. |
| **6. `gpu-rogue` on the RV2** | The BXE-2-32's MIPS firmware, the kernel CCB, a client CCB per lease, and a NULL job whose update the app waits on. The same app API through a server queue. |
| **7. Costs and the exit** | Submit-to-completion latency and calls per job on both boards, flip latency, and the kill tests under GPU load. |

Steps 2 to 4 are independent of each other and can run in any order. Step 5 needs steps 1 and 2. Step 6 needs step 1, and the K1's power domain from step 2.

## 1. The kernel calls queue leases need

**Done (2026-10-20).** Five calls, 46 to 50, on both ISAs and every QEMU boot.

- **Futexes.** `WaitAddr(address, value, deadline)` sleeps while a 64-bit word holds `value`, and `WakeAddr(address, count)` wakes up to `count` sleepers and returns how many it woke. The deadline is `CurrentTime`'s, and `ERR_TIMEOUT` (new, -13) says it passed. Words are keyed by physical address, looked up in the caller's own domain, so this works across processes in the identity layout. The waiter is listed before it reads the word, so a wake can't fall between the read and the sleep. The word is read through the caller's mapping, never the direct map, because it may be in an `AREAF_NC` Area. A wait may end early, and so does every wait on an Area when any mapping of it goes, including at a death (§12.5's rule, carried out more widely). Never for cross-process locks (§12.5): for sequence numbers only. `kernel/futex.c`.
- **A page of a lease for a peer.** `ShareLease(lease, address, size, task)` maps whole pages of the grantee's registers into a connected peer's process as device memory, and `UnshareLease(lease, address, task)` takes them back. They go anyway with the grant (the grantee's death) or with the peer. Refused: the grantee itself, a process it isn't connected to (`ERR_RIGHTS`, as `ShareArea`), pages outside the lease, and the same page twice (`ERR_EXISTS`). There are at most `LEASE_SHARES` (32) shares per lease, which bounds decision 6 per GPU. The peer gets no line and no DMA. `QueryLease` counts the shares.
- **Cache attributes. Corrected: one flag, `AREAF_NC`,** not `AREAF_NC` and `AREAF_WC`. On both ISAs the uncached memory type for RAM is the one whose writes may combine: Arm Normal-NC (MAIR index 2, already set) and RISC-V Svpbmt's NC. The only stronger type is Device memory, which faults on the unaligned accesses `memcpy` makes. Every mapping of an NC Area is NC. The kernel's direct map still caches its pages, so the kernel flushes the alias as the Area starts (it was zeroed through it) and as it ends. RISC-V needs Svpbmt, which the kernel finds by trying it in a scratch domain (a faulting kernel read goes to the copy's fixup), because the K1's U-Boot tree says only `rv64imafdcv` though the X60 has it. QEMU's CPU gets `svpbmt=true`. Without it, `ERR_NOSYS`. NC with `AREAF_EXEC` is refused.
- **A device's address window.** `LimitLeaseDma(lease, limit)`, by the owner while the lease is idle: every DMA Area of the lease then ends at or below `limit`. hwdb takes the limit from the identity part of the `dma-ranges` that apply to the node: those of the bus its `interconnects` name first (the K1's `dram_range` buses), or else of the nearest bus above it. hwdb records it on the node (`dma below` in the report) and gives it to the lease. U-Boot's tree has no `dram_range` buses, so a `ky,x1` quirk (`HW_Q_DMA_LIMIT`) gives the DPU 2 GiB. The GPU's node isn't in that tree at all: step 6 adds it, with the same quirk.

**Tested by** `conformance/futex.c`: the refusals; a word already moved; deadlines to come and passed; wake counts over three tasks; 2,000 round trips of a sequence number in a connection's arena, between two processes (35–46 µs each on QEMU TCG); a waiter killed; and one woken by its peer's death. `conformance/uncached.c` covers an NC Area zeroed, written and read, and refused as code. `conformance/doorbell.c`, driven by `leaser.c`, covers:
- each refusal of a share;
- the virtio device read through a shared page;
- the page unshared and shared again;
- a peer that faults on the page once the grantee has died;
- a DMA limit with room for one page that allows one page and not a second.

hwdb's own tests check `dma-ranges` on a hand-built tree of the K1's shape, and the quirk. Mutation-tested: without the limit, or without revoking shares at the grantee's death, the leaser fails.

**Not yet on the boards.** On the RV2 the Svpbmt probe works (step 2's run: `V5.1 uncached ok`), and the DPU's limit is untried, and on the CIX nothing here touches hardware until step 5. Both boards boot this build at the next step that needs them.

## 2. `power`

**Done (2026-10-21), on QEMU and both boards.** `drv/power` is a driver like the others, with a backend per kind of hardware, and hwdb votes through it before granting a lease.

- **The class** (`planar/devices/power.h`). One port, `power`. `POWER_ON` names a set and a tag of the client's own. A set is either the firmware references of a node (kind, which of power's devices, and the cells) or a name such as `"gpu"`. `POWER_OFF` takes a vote back, and `POWER_QUERY` reads a set from the hardware. A vote turns domains on first, then clocks, then releases resets. It is all or nothing: if one resource fails, the ones it turned on go off again. A resource goes off when its last vote goes, in reverse order, and only if power turned it on: whatever was on when power started is kept. A reference power doesn't drive is left as the firmware set it and counted (`pm_Untouched`); it isn't an error.
- **Votes are per connection** (§11.5). When a client's connection goes, its votes go. The driver library gained a `gone` callback for this.
- **Power before lease** (D3, `servers/hwdb/power.c`). A unit whose nodes reference power's devices waits in `U_POWERING` until power answers its vote, and only then are its leases granted. hwdb never waits (D6). The vote goes once the unit is torn down and its leases revoked, so each restart votes again. If power dies, its votes die with it: once it is back, hwdb connects again and votes for every unit it had voted for.
- **The K1** (`k1.c`), from the vendor's `x1-pm_domain.c`, `ccu-ky-x1.c` and `reset-ky-x1.c`:
  - Its seven power domains. The display's and audio's are switched by the hardware; the rest go through the software sequence, with the vendor's delays and its 60 ms limit.
  - The gates of the clocks and resets V5's devices use: SD, display, HDMI, USB3 and the GPU.
  - It drives only what is in the APMU (`0xd4282800`), so hwdb leases power that one page (`HW_Q_REGS`). The other seven blocks the clock controller lists stay as U-Boot set them; they hold the kernel's watchdog register, the DDR controller and PCIe. PCIe's and the combo PHY's words in the APMU page are dropped from their nodes too, until step 4 has power drive them.
  - U-Boot's cells: clock numbers are Linux's plus 41, reset numbers are Linux's, and domain numbers are mainline's.
  - The GPU has no node, so `"gpu"` names its domain, its clock and its reset.
  - The clock and reset controllers are no longer the firmware's, so SD now waits for power to bind.
- **The CIX** (`scmi.c`): native SCMI over both of its transports.
  - Clocks go to the SCP through SHM0 at `0x6590000`, rung at mailbox 6's `+0x80` and polled. Only `0x00`–`0x80` of that page are touched: the rest is the CPPC's and the PCC's.
  - Power domains go to TF-A by SMC `0xc2000001`, through the channel at `0x84380000`. Only the device tree names it, so a hwdb quirk adds it to SHM0 (`HW_Q_MMIO`), and a kernel quirk lets that reserved memory be leased.
  - The mailboxes all say `CIXHA001`, so MBX6 is pinned to power by name.
  - Every channel access is 32 bits wide.
  - Domain and clock IDs are the vendor tree's. ACPI names none of them, so only named sets reach them: `"gpu"` is domain 21 and `gpu_clk_core` and `gpu_clk_stacks`.
- **Two kernel calls, 51 and 52.**
  - `SetLeaseCall(lease, fid)`: the owner, while the lease is idle, names the one firmware function its grantee may call. hwdb takes it from a quirk (`HW_Q_CALL`).
  - `CallLease(lease, x[4])` makes that call by SMCCC, through the conduit PSCI uses.
  - `QueryLease` gives the function as `li_Call`.
  - RISC-V answers `ERR_NOSYS`.
- **A device hwdb makes with no registers** (the model, below) gets a lease of no ranges (`lease_bare`), and the driver library accepts one. Any other device without registers still fails, as it did: two of the CIX's host bridges have no ECAM. The first board run gave every such device a lease, and their `pci` failed until given up.
- **QEMU's model.** QEMU has nothing to power, so hwdb's test mode adds a node, `planar,power-model`, with no registers, and has the RTC need its domain 1. Under edk2, which keeps the RTC, the display needs it instead. The model's resources are bits in memory. Domain 0 starts on, and domain 3 never comes on, to exercise the error path.

**Tested by** `make power-test` on the host (62 checks):
- the core on the model: order, shared votes, tags, refusals, a full table, a vote failing half way, a client going, and the firmware's resources kept;
- the K1 backend on a made-up APMU whose status follows its control bits;
- the SCMI backend against a made-up agent on both channels. The agent checks each message's header, token and length, and that the words of mailbox 6's page above the doorbell are untouched.

In the system, on every QEMU boot, hwdb's kill test:
- votes before the RTC's lease (or the display's), and checks that each restart after a death turned on again what the first vote did, so the death took the vote back (9 to 24 restarts a boot);
- has `conformance/powervote.c` check its refusals, vote for `"gpu"`, and be killed, then checks that `"gpu"` went off;
- checks that a vote that can't be met is refused;
- kills power, and checks that every driver is voted for again.

`leaser.c` and `doorbell.c` test `SetLeaseCall`/`CallLease` with PSCI_VERSION.

**Corrected:**
- The G720's five steps aren't one `PowerOn(gpu)` in power. Only the first two are SCMI's: domain 21, then the clocks `0x1F` and `0x20`. The other three move to step 5, because they need GPU-side registers and power has no lease on those:
  - the power resource `GPUP.PPRS._ON` sets bits 2–12 of RCSU `0x15000218` and runs memory repair at `0x15000010`–`0x18`;
  - the reset pulse at `0x16000400` bit 6 (a register it shares with the DDR controller, the GIC and the NoCs: read-modify-write only; vendor Linux holds it 10–20 ms, FreeBSD 20 µs);
  - the Q-channel enable, bit 0 of RCSU `0x218`.
- Power never turns off what the firmware left on, and after a restart that includes what the last power turned on. That wastes power and breaks nothing. Linux's `clk_disable_unused` is the opposite policy, and is known to break boards whose consoles or buses nobody claims.
- The RV2's GPU domain is probably mainline's 1 under U-Boot's numbering, not the vendor kernel's 2. Its clock's number, `0xbd`, is inferred by the plus-41 rule.

**On the RV2** (2026-10-21, `make PLATFORM=ky-x1 test`):
- power bound the APMU page for its three nodes;
- SD bound behind it and booted the disk, dos and the Shell as before;
- the kill test went through 24 kills;
- the client's vote turned the GPU's domain, clock and reset on (the domain's status bit came up within the vendor's limit), and its death turned them off;
- power was killed and the votes made again.

No restart turned anything on again, as expected: SD's clocks and resets were U-Boot's, so they're kept. The same run confirmed step 1's Svpbmt probe: uncached Areas work on the X60.

**On the CIX** (2026-10-21, `make PLATFORM=uefi-a64 test`):
- Both transports answer. The SCP gives clocks at SCMI 2.0 through mailbox 6, and TF-A gives power domains at 2.1 by SMC with zeros in x1 and x2, as Linux passes them.
- The kernel leased the SMC channel's page.
- A client's vote for `"gpu"` turned on domain 21 and the one GPU clock that was off. One of the three was already on, so it's kept. Its death turned them off.
- Power was killed and came back.

No CIX driver references power: ACPI names no SCMI resource, so hwdb votes for nobody there.

## 3. Native scanout

**Done (2026-10-21), on QEMU and both boards.** `display` takes over the mode the firmware left, without setting one, draws the text console on plane 0, and gives clients an overlay and a cursor plane above it.

- **The class** (`planar/devices/display.h`). Two ports on one output. `con.0` is the console as before. `display.0` handles the planes:
  - `DISPLAY_INFO` gives the size, the planes' formats and limits, the sync page, and how many buffers are allocated.
  - `DISPLAY_ALLOC` makes a buffer, as a DMA Area of display's lease that the controller can reach, mapped into the client.
  - `DISPLAY_COMMIT` sets some planes at once and answers at once with the commit's number. The controller takes it at the next latch, and commits between two latches go together.
  - `DISPLAY_FREE` frees a buffer the screen no longer shows.
  - A plane belongs to the client that first commits to it. Plane 0 is the console's.
  - **The sync page** (§16.3 pillar 4) holds `ds_Vblank`, `ds_Time`, `ds_Disp` and `ds_Safe`, each woken by `WakeAddr`. On these controllers `ds_Safe` moves with `ds_Disp`.
- **Buffers are the server's**, so a client can't take one from under a scanout (§16.6).
  - When a client goes, its planes go off at the next latch, and its buffers are freed after that latch.
  - When display goes, the kernel holds the buffers a client still maps, counted as the lease's quarantine. hwdb's stop method turns every plane off but the console's and waits for the latch. RevokeLease then makes each held buffer an ordinary Area, freed when the client lets go (`area_hold_lent`, `lease_dma_lent`).
- **Restart without a blank.** hwdb's stop method never resets the controller (`servers/hwdb/display.c`, with `komeda.h` and `saturn.h` shared with display). The restarted display gets `DRVF_AGAIN`, finds the console's text still there, and goes on over its bottom line.
- **The K1** (`saturn.c`), the HDMI DPU at `0xc0440000`, pipeline 2:
  - U-Boot's layer is RDMA 3 in composer slot 0. display moves it to slot 7, the bottom.
  - The overlay is RDMA 2 in slot 6, and the cursor RDMA 0 in slot 0.
  - The line buffer RDMAs 2 and 3 share, which U-Boot leaves overlapping, is split between them.
  - A commit ends with `CTL2_CFG_RDY`, which the hardware clears at the latch.
  - The vblank is the vsync interrupt, line 139, added by a quirk because U-Boot's tree lists none.
- **The CIX** (`komeda.c`), DPU4, a Linlon-D60:
  - display walks its blocks.
  - The planes are layers the GOP left off, fed into compiz inputs above the GOP's.
  - A commit ends with `GCU_CONFIG_VALID0`. The vblank is the DOU's PL0.
  - It is never reset. DPU0–3 are excluded.
- **QEMU** (`fb.c`): the framebuffer alone. The console draws in a copy, and display composes the planes over it at 60 ticks a second. Its buffers are DMA Areas too, so the held-buffer path runs on every QEMU boot.
- **hwdb**: `HW_Q_SCANOUT` leases a controller and the framebuffer it scans out to one process; `HW_Q_IRQ` adds a line the firmware doesn't list.
- **The driver library**: a device may publish two ports (`DRV_PORTS`), and `DRVF_AGAIN` tells a driver that it's a restart.

**Tested by** `make display-test` on the host (89 checks):
- each DPU backend against made-up registers laid out as the firmware leaves them, with a thread standing in for the latch;
- commits latched at the vblank and not before, and coalescing;
- clipping at the screen's edges;
- the refusals;
- a client's going;
- hwdb's stop method leaving the firmware's plane alone;
- fb.c's composition and blending.

On every QEMU boot under edk2, and on both boards, the kill test's `T_DISPLAY` phase runs `conformance/displaytest.c`:
- the refusals, then 30 flips of the overlay with the cursor moving partly off the screen, each waited for on `ds_Disp`;
- the client killed, and its buffers checked freed;
- planes shown again, display killed under them, the client finding its buffers still writable and display back with none of them.

The kill test also kills display as the console's driver, and uses it for the give-up case on the boards.

**Corrected:**
- **Neither DPU has a cursor in hardware.** The cursor is an ordinary plane on top, clipped by display (§16.7 says otherwise).
- **The CIX's latch is `GCU_CONFIG_VALID0`**, not `LPU_FLUSH` (§16.3). On the K1 it's `CTL2_CFG_RDY`.
- **The CIX's DPU isn't coherent** (`_CCA` 0). The AArch64 `drv_dma_clean` and `drv_dma_flush` only ordered writes, on the belief that Planar's Arm machines snoop. They now clean and invalidate by address. The kernel lets EL0 do so and read `CTR_EL0` (`SCTLR_EL1.UCI` and `UCT`).
- **The boot framebuffer's lease is mapped cached**, not as a device. The console cleans what it draws. Reading a 1920×1080 screen back through a device mapping, to scroll, took seconds on the CIX.
- **The kernel's wait for hwdb's test** went from 15 s to 40 s. The boards' kill test takes most of 15 s by itself.

**On the CIX** (2026-10-21, `make PLATFORM=uefi-a64 test`):
- The GOP's mode is 1920×1080 at 60 Hz, one layer straight into compiz input 0, with no scaler.
- Each flip was on the screen about two frames (33 ms) after its commit.
- The screen, checked by eye, was right. The first run showed stale memory on the top half: the cache wasn't cleaned.

**On the RV2** (2026-10-21, `make PLATFORM=ky-x1 test`):
- U-Boot's mode is 1920×1080 at 60 Hz.
- Each flip was on the screen one frame (16.5 ms) after its commit, as omega measured.
- The console's move to the bottom slot didn't show. The screen, checked by eye, looked like the CIX's.

## 4. USB input

**Done (2026-10-21), on QEMU and both boards.** A USB keyboard drives the Shell, and keys and the mouse reach an `input` client on a ring it maps.

- **The class** (`planar/devices/input.h`, [Devices ch. 7](../manuals/devices/07-input.md)). `input` (`servers/input`) is one server for every keyboard and mouse, with no device of its own. hwdb starts it before the drivers, in both modes, and again whenever it ends.
  - Drivers connect to "input" and send `INPUT_ATTACH`, `INPUT_REPORT` (raw boot-protocol reports) and `INPUT_DETACH`.
  - A driver's connection going takes its devices with it, and lets go of every key they held.
  - One client takes the ring (`INPUT_OPEN`): a page mapped read-only, whose `ir_Head` counts every event and is woken with `WakeAddr`. Another client meanwhile gets `ERR_BUSY`.
  - Events are keys (HID usages, with what each types on a US layout), buttons and moves, with the modifiers held. `input` repeats a held key, and keeps Caps Lock itself.
- **`xhci`** (`drv/xhci`, xHCI 1.2, about 1,600 lines): one process for every controller it's given.
  - Bind takes a controller from the firmware (USB Legacy Support), resets it and starts it. Its rings, contexts and buffers are in uncached DMA Areas below 4 GiB.
  - From the loop, each USB 2 port with something on it is reset, and its device addressed and configured. Hubs (USB 2.0 ch. 11) have their ports powered and looked at every 250 ms. A full- or low-speed device behind a high-speed hub goes through its single TT.
  - A boot-subclass keyboard or mouse is put in the boot protocol, with 8 transfers queued on its interrupt endpoint. Each report goes to `input`.
  - SuperSpeed ports aren't walked.
  - Interrupts come on the line, or MSI-X vector 0. QEMU's RISC-V PCI is polled.
  - hwdb's stop method halts each controller (`servers/hwdb/input.c`).
- **The Shell** reads keys from `input`'s ring as well as the serial console, and takes the ring again after `input` restarts.
- **QEMU**: `qemu-xhci` with a hub, a keyboard and a mouse behind it (`QEMU_USB`). `tools/boot-check.py` types through QMP what the test asks for.
- **hwdb**:
  - `HW_Q_NOIRQ` drops a node's lines through a controller (the CIX's PDC0), or one line (the K1's DWC3 glue's wakeup line).
  - A reference to a node described twice is to the device it aliases.
- **power**: a fourth kind, `POWER_SUPPLY` (a GPIO driven high). On the K1, the set `"usb"` holds the combo PHY's mode and reset and the hub's VBUS. Resets may have bits that clear to release.
- **drv.library**: `drv_io_barrier`, ordering memory a device uses against its registers on both ISAs. A compiler's fence doesn't order I/O on RISC-V.

**Tested by** the kill test's `T_INPUT` phase (`servers/hwdb/input.c`, with `conformance/inputtest.c`), on every QEMU boot and both boards:
- the keyboard and mouse found, then a client checks what's typed ("Hi 5" and Enter, Shift held over the H), a move of (10, −5) and a click;
- again after every xhci was killed and came back;
- again across `input`'s own death, the client reconnecting to a new ring.

On the boards nobody types for the test, which says so ("typing not exercised"); the Shell was typed in by hand.

**On QEMU**: everything passes on all ten boots, with MSI-X on AArch64 and polling on RISC-V.

**On the CIX** (2026-10-21):
- All ten controllers start: Cadence CDNSP, xHCI 1.10, 64-byte contexts. hwdb gives xhci their `CIXH2031` descriptions, whose `_STA` doesn't read GNVA, so VQ42 no longer keeps three of them off.
- The keyboard and mouse are on `USB1`, behind a single-TT hub (1a40:0101) at high speed, as FreeBSD saw them. Every key typed in the Shell came through.

**On the RV2** (2026-10-21):
- The USB-A ports are behind the onboard Genesys hub, on the DWC3. The core needs the combo PHY's pipe clock for an xHCI reset to end, even with only USB 2 walked, so xhci starts that PHY. The hub's ports get VBUS from GPIO 123, which U-Boot's tree gets wrong (79) and U-Boot leaves low.
- A low-speed keyboard and mouse work through the hub's TT.
- With one transfer queued at a time, keys were lost: the endpoint went unpolled while the driver was late. 8 queued fixed that.
- The board's other USB-A port is the OTG controller's (an EHCI, VBUS GPIO 126), which isn't driven.

## 5. `gpu-panthor` on the CIX

**Done (2026-10-21), on the CIX.** An app takes a queue of its own from `gpu.0`, writes CSF instructions into its own ring and rings its own doorbell. The doorbell-to-update time for the first job is 25–50 µs. Only 2 kernel calls are made machine-wide meanwhile, both the test's own timing. 1,000 jobs take 11 µs each, and a job waited for with `WaitAddr`, woken by the server, takes 16 µs. The class is [Devices ch. 9](../manuals/devices/09-gpu.md).

- **The class** (`planar/devices/gpu.h`): `GPU_OPEN`, `GPU_MAP`/`GPU_UNMAP` (a whole `AREAF_NC` Area the client shared with the server, mapped at its own address in the client's own GPU address space), `GPU_QUEUE` (the ring's input and output pages and the doorbell page, shared with the client), `GPU_STATUS` (faults).
- **The driver** (`drv/gpu-panthor`, about 1,600 lines): bring-up from the loop, since nothing may read the GPU before it has power; the firmware read through `dos`; the MCU's address space and one per client; one group per client, pinned to a slot with its doorbell, as panthor numbers them (slot *n*, address space and doorbell *n* + 1); the job line's events (sync updates woken, stream faults); the 12 s ping.
- **hwdb**: the G720 matched (`CIXH5000`); RST0's page added to its lease, and taken from PDC0, RST0 and CRU0, which nobody drives; a new quirk, `HW_Q_POWER`, which has hwdb vote for a named set before the lease; `stop_gpu` (MCU off, soft reset, unless it was never powered or is held in reset); and the kill test's `T_GPU` (`servers/hwdb/gpu.c`, with `conformance/gputest.c`).
- **The firmware** is copied from `/Volumes/nas/firmware` into the build, checked against its SHA-256, and installed as `DEVS/mali_csffw.bin` with each CIX run (`mk/uefi-a64.mk`; decision 5).
- **A kernel call, 53: `ReleaseArea(address)`.** A process lets go of an Area another process shared with it. Only an Area's owner could take a share back, so what a dead client had lent `gpu-panthor` would have stayed mapped in it, held, until the server died. Tested by `conformance/conn.c`.
- **drv.library**: `every_line` binds all of a device's own lines, and hwdb now gives `dd_NLines` for a device with several (the G720's job, MMU and GPU lines).

**Tested** on the CIX by the kill test's GPU phase, three rounds:
- `gputest` takes a queue and runs a job, 1,000 more, then one waited for;
- a second `gputest` running jobs without end is killed under them, and a third round runs after it;
- `gpu-panthor` is killed under that third client, which connects to the restarted server and runs a round again.

The kill rounds kill `gpu-panthor` too, during bring-up and after, with no client. On QEMU the phase says "not exercised: no GPU here".

**Found on the board:**
- **UEFI leaves the GPU held in reset**: RST0 `0x400` bit 6 is clear. Until it's released, the first GPU register read is refused and the CPU takes an SError. The driver pulses it on every start, after power and before reading, as the vendor's tree does: 20 ms each way, read-modify-write, since the word is also the DDR controller's, the GIC's and the NoCs'.
- **Pulsing the RCSU's reset (`0x800` bit 28) breaks the RCSU.** TF-A's interconnect monitor then refuses every access to it ("IDM: GPU secure access"). Only the GPU's reset is pulsed.
- **A refused access wedges the board.** TF-A prints IDM errors without end, and the watchdog can't reset it; only a power cycle does. So nothing may touch a GPU register unless the GPU has power and is out of reset:
  - The driver's own vote for `"gpu"` went with its death, before hwdb's stop wrote to the GPU, so hwdb votes instead (`HW_Q_POWER`), and its vote goes only after the stop.
  - A driver killed during its reset pulse leaves the GPU held in reset, so `stop_gpu` leaves a GPU in reset alone.
- **The extract pointer is lazy.** The firmware moves `gq_Extract` when it gets to it, not at each job.
- The CSF's global interface reports version 1.1, with 8 group slots. The address spaces number 8, so 7 queues are given to clients.

**Corrected:**
- The plan's five power-on steps are four on this firmware: power's set, GPUP's `_ON`, the reset pulse, and the Q-channel. The memory repair runs only if RST0's strap (`0x504` bit 4) asks, and it didn't on this board.
- Mesa's `genxml` wasn't needed: V5's job uses only `MOV48`, `SYNC_ADD64` and `ERROR_BARRIER`, whose encodings are panthor's own. `SYNC_WAIT` waits for V6.

**Not yet:**
- More groups than slots; compute and the tiler heap (V6).
- A fault reaching the client as a message: it's kept for `GPU_STATUS`, and the client's sync object doesn't move.
- The step 7 costs under load.

**This is where Planar goes past Linux.** panthor doesn't map doorbells into user space: the kernel writes the ring and rings the doorbell for every job. Planar gives the app its queue instead.

- **Doorbells have a 64 KiB page each:** `CSF_DOORBELL(i)` at `0x15090000 + i × 0x10000`. Doorbell 0 is the global interface and stays with the server. The rest go one per app, through step 1's sub-lease.
- **One pinned group slot per lease.** A group that moves to another slot gets another doorbell. In V5 a group never moves, and the number of leases is the number of slots. Scheduling more groups than slots is later.
- **The app holds its ring, its queue's input and output pages, and its doorbell.** The server holds the firmware, the global interface, the MMU and faults.
- **Bring-up,** after `PowerOn(gpu)`: power the L2, give the MCU its own address space (the firmware's addresses are fixed, so not identity), load the firmware's sections, boot the MCU and wait for the global interface, and set it up. Reference: panthor's `panthor_fw.c`, lines 1434 to 1489.
- **The firmware** is `mali_csffw.bin` (the vendor kernel's flat name for `arm/mali/arch12.8/`), interface 3.13: 266,240 bytes on the NAS (decision 5), SHA-256 `267f958f…`, CSF header major 0 and minor 1, built from git `e6cfd856`. The header doesn't say its arch; the GPU's `GPU_ID` check at boot does.
- **The app's address space is identity:** the GPU's MMU is Arm LPAE with 4 KiB and 2 MiB pages, a 48-bit VA and a 40-bit PA, and RAM is below 2^40. `gpu-panthor` maps each Area the app grants it at its own address (§16.5). The server's own objects sit at addresses no Area can have.
- **No SMMU** confines the G720 (none in IORT): its MMU is the only confinement, and the lease records that (VQ9).
- **A job.** The smallest that can be observed is `MOV48` the sync object's address, `MOV48` 1, `WAIT`, `SYNC_ADD64`, then `ERROR_BARRIER` (opcodes in panthor's ring preamble, `panthor_sched.c` lines 3214 to 3263). The sync object is `{u64 seqno; u32 status; u32 pad}`. The app spins on `seqno`, or sleeps in `WaitAddr`.
- **A wait in a queue** (`SYNC_WAIT`) makes it idle as `CS_IDLE_SYNC_WAIT`, which the server must watch. The encodings of `SYNC_WAIT64` and `SYNC_SET64` are in Mesa's `genxml`, which isn't here (decision 5).
- **Faults and death** (§5.4). A fault becomes a message to the lease's holder, and the server terminates that group alone. When the holder dies, the server terminates its group and unmaps its address space before its Areas leave quarantine.
- **The firmware's watchdog** pings every 12 s, and the server must answer it.
- **Compute only.** No tiler heap: that's V6's, with graphics. About 3,000 to 4,000 lines, against panthor's 15,200.

## 6. `gpu-rogue` on the RV2

**Done (2026-10-22), on the RV2.** An app takes a queue from `gpu.0`, writes a NULL command and an update of its sync object into its own client CCB, and asks `gpu-rogue` to kick it (`GPU_KICK`): the same API as the CIX's, through a server queue. The first job, kick to update, takes 52 µs, with 5 kernel calls made machine-wide meanwhile. 1,000 jobs take 54 µs each, and a job waited for with `WaitAddr`, woken by the server, takes 55–57 µs. The class is [Devices ch. 9](../manuals/devices/09-gpu.md).

- **The class** gains `GPU_KICK`, `gm_Kind` (`GPU_CSF` or `GPU_ROGUE`, from `GPU_OPEN`; it was padding) and `gm_Device` (from `GPU_MAP`: where the GPU sees the Area). A zero `gm_Doorbell` means the server queue. `gpu-panthor` sets both new fields.
- **The driver** (`drv/gpu-rogue`, about 1,400 lines): the core's ID; mainline's firmware loaded into the MIPS's 16 MiB heap through a 4-page table, its structures at their fixed offsets, the MIPS booted by mainline's sequence; the kernel CCB (kick, MMU flush, cleanup, health check) and the firmware CCB; one firmware context a client, on the GP data master, with the client's ring as its client CCB; the wrapper's interrupt, which moves each `gq_Extract` and wakes sync objects.
- **hwdb**: a new quirk, `HW_Q_NODE`, adds a device the firmware's tree leaves out: U-Boot's has no GPU. The RV2's quirks give it its registers, line 75, the `"gpu"` vote and the 2 GiB DMA limit; `gpu-rogue` matches `img,rgx`; `stop_rogue` soft-resets the GPU, which holds the MIPS, before a dead server's memory leaves quarantine. The kill test's GPU phase runs it with `gputest`, which writes Rogue's commands when `gm_Kind` says so.
- **power**: the GPU clock's rate is set once its gate is open and its reset released. Core now says which resource didn't turn on.
- **The firmware** is copied from the NAS and checked as the CIX's is, and installed as `DEVS/rogue.fw` by `make PLATFORM=ky-x1 sdcard` (`mk/ky-x1.mk`).

**Tested** on the RV2 by the kill test's GPU phase, the CIX's three rounds. The kill rounds kill `gpu-rogue` during bring-up and after, and it boots the firmware again each time.

**Found on the board:**
- **The clock's frequency change never finishes while the GPU is held in reset.** Power opened the gate, set the rate and then released the reset, as its kind order says; the change bit (APMU `0xcc` bit 15) stayed set. The vendor's driver releases the reset first (`ky_init.c`). Power's K1 backend now sets the rate after whichever of the two goes on last.
- **A health check answers in no slot.** The firmware reads it, and its count of kernel CCB commands done (`osdata`, offset 568) goes up, but its slot's return word stays 0. Mainline's watchdog uses the count, and only waits on a slot for cleanups, MMU flushes and log changes; so does `gpu-rogue`.
- **The firmware interrupts after each update.** `WaitAddr` is woken by the server within 2–3 µs of the spun-on time.
- Idle, the MIPS sleeps (`MIPS_EXCEPTION_STATUS` `SI_SLEEP`), and its connection state is `ACTIVE` once it's booted.

**Corrected:**
- **The plan said hwdb already sees the GPU.** U-Boot's tree has no GPU node: hwdb adds one (`HW_Q_NODE`).
- **The vendor's 1 MiB window** overlaps Ethernet at `0xcac80000`: the GPU's is 512 KiB.
- **Apps' Areas aren't in the GPU's MMU.** V5's job reads no GPU memory, so a context's page catalogue stays empty, and what a client lends is mapped in the firmware's heap instead, where the firmware reads the ring and writes the UFO (`gm_Device`). The GPU's own three-level MMU comes with jobs that read memory (V6).
- About 1,400 lines, not 3,000 to 5,000.

**Not yet:**
- The GPU's MMU, and compute (V6).
- A context reset reaching the client as a message: as on the CIX, it's kept for `GPU_STATUS`.
- The step 7 costs under load.

The BXE-2-32 is Rogue core **36.29.52.182**, with a MIPS firmware processor. The vendor's DDK (24.2) drives it: `drivers/gpu/drm/img-rogue` on the `orange-pi-6.6-ky` branch. But the firmware on hand is **mainline's**: open-source firmware 1.0 (build 6734358) for exactly this core, in mainline's container. So `gpu-rogue` follows mainline's `imagination` driver and its interface (`pvr_rogue_fwif.h`), which doesn't list the core in its device table yet, and takes the K1's platform facts from the DDK.

- **No user doorbell.** A job is kicked by a kernel CCB command and a write to `RGX_CR_MTS_SCHEDULE`, a privileged register. So the provider is a server queue (§5.1): the app writes its client CCB, which is mapped to it, and asks `gpu-rogue` to kick it. The app's API is the same as on the CIX. Only the cost differs, measured in step 7.
- **The firmware** is `rogue_36.29.52.182_v1.fw`: 126,976 bytes on the NAS (decision 5), SHA-256 `79e465ea…`. A MIPS ELF, then in its last 4 KiB mainline's info header (`pvr_fw_info.h`: version 3, six layout entries, `PVR_FW_FLAGS_OPEN_SOURCE`). `pvr_fw.c`'s `pvr_fw_validate` shows how to check it. The DDK's own `rgx.fw` isn't needed.
- **Bring-up:** step 2 powers domain 2 and sets the clock to 614.4 MHz. Then a soft reset, the MIPS boot remap windows (`ADDR_REMAP1` to `5`), the firmware's image laid into its heap at the fixed GPU address `0xE1C0000000`, and the kernel CCB. Reference: mainline's `pvr_fw.c`, `pvr_fw_mips.c`, `pvr_fw_startstop.c` and `pvr_ccb.c`; the DDK's `rgxstartstop.c` and `ky/` for what's the K1's own.
- **Two MMUs:** the MIPS firmware's single-level table, and the GPU's three levels with a 40-bit VA. Apps' Areas map at their own addresses, from `AREAF_DMA32` memory (step 1).
- **No snooping.** Everything the GPU reads is cleaned from the CPU's caches first.
- **The job is a NULL command,** or a kicksync: a fence and an update around no work. Completion is a UFO, `{address, value}`, which the firmware writes and then interrupts. The app waits on it as on the CIX. A compute job would need a hand-built USC shader with no compiler, and isn't in V5 (decision 1).
- **About 3,000 to 5,000 lines.** The risks, in order: a core mainline hasn't run (its feature and quirk set comes from the DDK's `rgxcore_km_36.29.52.182.h`), the MIPS boot, the address window, and coherency.

## 7. Costs and the exit

- **Calls per job:** 0 on the CIX between doorbell and completion, and the server's crossings on the RV2.
- **Latency** from submit to the sync object's update, spinning and with `WaitAddr`.
- **Flips:** from commit to latch, on both boards, against the vblank interval (omega's `selftest.c` method).
- **The kill tests** kill `gpu-*`, `display`, `input`, `power` and the GPU clients, under load.

## Sources

- **The CIX:** `drm-panthor-kmod` (FreeBSD's panthor, with the power-on glue), `drm-komeda-kmod`, `freebsd-src/sys/arm64/cix`, AbyssBSD's notes for the Orange Pi 6 Plus, the board's own DSDT (`boards/cix-p1/acpi`), and the vendor kernel's `orange-pi-6.6-cix` branch.
- **The RV2:** mainline's `imagination` driver (the firmware's interface), the vendor kernel's `orange-pi-6.6-ky` branch (`img-rogue` for the core's facts, `ky`, `soc/ky`, `clk/ky`), and omega's `plat/ky-x1`.
- **Firmware:** `/Volumes/nas/firmware`, which has `mali_csffw.bin` and `rogue_36.29.52.182_v1.fw`.
- **Not found locally:** Mesa's `genxml` for CSF (not needed in V5, step 5).

## Carried in from V4

- **`power`** and the kill test's check of votes (V4 step 7).
- **Native scanout** (V4 decision 2).
- **USB keyboards** (V4 decision 1).
- **VQ42**, GNVA: three of the CIX's xHCI controllers' `_STA` depend on it. Step 4 reaches them through their other description, CIXH2031.

## Corrections to the design

- **§5.1 and §5.3:** panthor has no user doorbells, and Planar's are its own (step 5). §16.5's "zero kernel trap" holds only on hardware with doorbell pages, and the RV2 isn't one.
- **§3.3:** `AREAF_WC` folds into `AREAF_NC` (step 1).
- **§5.2:** the G720 snoops one way only, and its rings are non-cacheable to it. The K1's GPU and DPU snoop not at all.
- **§16.7:** the CIX's G720 has no SMMU, and the RV2's GPU and DPU see memory above 4 GiB at another address.
- **VQ17:** vendor sources exist (DDK 24.2). Whether the 2 TOPS are in the cores is still open.

## Decisions

1. **The RV2's GPU.** In V5 with a NULL job through a server queue; in V5 with a compute job; or later, on CPU SIMD until after V6. **Decided (2026-10-20): in V5, a NULL job.** The exit proves one API over two providers.
2. **The NPU.** The roadmap put the Zhouyi NPU at the end of V5. **Decided (2026-10-20): moved out,** to V7 with the RK3588's RKNN, as a second server-queue provider.
3. **USB input.** **Decided (2026-10-20): in V5,** so V6 starts with a keyboard and mouse.
4. **The Workbench phase.** **Decided (2026-10-20): V6**, with RTG (§16) and `font.library` (§14), before the other boards. The RK3588 becomes V7, the Q8B V8 and the PMP profile V9.
5. **Firmware and references from outside.** Both blobs are on the NAS, at `/Volumes/nas/firmware` (found 2026-10-20). Mesa's `genxml` (for CSF encodings, read only) is still to fetch. *Leaning:* blobs stay out of the repository. A tool copies them from the NAS onto each board's boot partition, checked against the SHA-256s above, and the GPU servers read them through `dos`, as programs are read now.
6. **How many leases.** One pinned group slot each means as many GPU clients as slots. *Leaning:* enough for V5's tests and V6's `intuition` plus a few apps. Scheduling groups over slots comes when something needs more.
