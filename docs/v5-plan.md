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

**Not yet on the boards.** On the RV2 the Svpbmt probe and the DPU's limit are untried, and on the CIX nothing here touches hardware until step 5. Both boards boot this build at the next step that needs them.

## 2. `power`

- **The CIX's SCMI has two transports.** Clocks (protocol 0x14) and performance (0x13) go to the CIXHA006 agent: shared memory is the first 0x80 bytes of mailbox 6 at `0x6590000`, rung at +0x80 and polled. Power domains (0x11) go to TF-A by SMC `0xc2000001`, with shared memory at `0x84380000`, an address the device tree has and ACPI doesn't (a quirk). Reference: FreeBSD's `sys/arm64/cix/sky1_scmi.c` and `cix_mbox.c`, about 600 lines.
- **Mailbox 6's page also holds the CPUs' CPPC fastchannel** (`0x659009c`). `power`'s lease must not hand it on.
- **The G720's five steps** become one `PowerOn(gpu)`: SCMI domain 21; clocks `0x1F` and `0x20`; the power resource `\_SB.GPUP.PPRS._ON` through acpid, which sets RCSU `0x15000218` and runs memory repair; reset `0x16000400` bit 6, pulsed for 20 µs; then Q-channel gating, RCSU `0x218` bit 0. A GPU register read before the last step is an SError, and the board needs a power cycle. RCSU may be read only at `0x218` and `0x304`.
- **The K1** has a PMU with a domain per block (GPU domain 2, display domain 6), and the CCU's clocks and resets. The GPU's clock and reset are both in APMU `0xcc`. Reference: the vendor kernel's `drivers/soc/ky/pm_domain/x1-pm_domain.c` and `drivers/clk/ky/ccu-ky-x1.c`.
- **Votes are per connection** (§11.5): a dead driver's go, and a domain with no votes goes off. That is the kill test's check, moved from V4.
- **Power before lease.** hwdb asks `power` before granting a node's lease, so a driver never sees an unpowered device.
- **What isn't touched.** The display controllers and xHCI stay as the firmware left them: neither needs SCMI. CPU and DSU performance domains aren't set in V5.

## 3. Native scanout

The firmware already lit one output on each board, and both drivers take it over rather than set a mode. Mode setting, EDID and a second monitor come later.

- **The CIX.** The monitor is on DPU4, through DP04 and a PS185 DisplayPort-to-HDMI converter. The DPU is komeda's D71 family. `display` walks its 0x200-byte blocks, finds the output block the firmware enabled and reads its timing, as `drm-komeda-kmod/freebsd/komeda_freebsd_bus.c` does. A flip writes a layer's `P0_PTR` and stride, then `GCU_CONFIG_VALID0`. Vblank is the output block's `DOU_IRQ_PL0`. **Never reset it:** a reset drops the DisplayPort link for good. About 300 to 500 lines.
- **The RV2.** The saturn DPU at `0xc0440000` has 4 RDMA channels and 8 planes, one of them a cursor. U-Boot scans out on RDMA 3. A flip writes its base address and `CTL2_CFG_RDY` (`0x56c`), which the hardware clears when it latches at frame start. Vblank is on line 139. The DPU's MMU stays off (`TBU_Ctrl = 0`, the vendor driver's `contig_mem` path). omega knows the same registers. Reference: the vendor kernel's `drivers/gpu/drm/ky`.
- **The interface `intuition` will use** (§16.3): a plane takes an Area, a rectangle and a format; a commit is latched at vblank; and each output shares a sync page with `disp_seq` and `safe_seq`, woken by `WakeAddr`. V5 builds the mechanism. The plane planner and screen dragging are V6's (VQ31, VQ39).
- **Restart without a blank.** A restarted `display` reads the current plane state back from the registers and goes on. While a plane shows an Area, `display` holds it, so a dying client's buffer isn't freed until a vblank has passed (§16.6).
- **The text console moves onto a plane.** The Shell looks the same.

## 4. USB input

- **xHCI** with control and interrupt endpoints, and no isochronous or bulk streams: about 1,500 to 2,000 lines (9front's `usbxhci.c` is 1,944).
- **Hubs from the start.** Both boards put one in the path: the RV2's onboard GL3523, and on the CIX a full-speed keyboard was seen behind a hub on `USB1` (`0x9298000`, USB 2 only). That needs the transaction translator's fields in the slot context.
- **HID boot protocol** for keyboard and mouse, with no report-descriptor parser: about 300 lines.
- **The RV2** sets its DWC3 to host mode (`GCTL`, `0xC110`) and powers its USB 2 PHY (the vendor kernel's `phy-k1-usb2.c`). VBUS is a GPIO, and U-Boot's tree and mainline name different ones (79 and 123). Try both, and record which.
- **The CIX** has ten xHCI controllers. hwdb leaves three of them disabled, since their `_STA` reads GNVA (VQ42). Which connector is which controller is still to be found.
- **`input`** is a server: it turns reports into `InputEvent`s on a ring to one client, the Shell now and `intuition` in V6. `GrantConsole` stays for the serial console.

## 5. `gpu-panthor` on the CIX

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
- **Not found locally:** Mesa's `genxml` for CSF.

## Carried in from V4

- **`power`** and the kill test's check of votes (V4 step 7).
- **Native scanout** (V4 decision 2).
- **USB keyboards** (V4 decision 1).
- **VQ42**, GNVA: three of the CIX's xHCI controllers depend on it.

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
