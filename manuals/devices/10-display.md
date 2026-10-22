# The display class

*Checked against V5.*

## What it's for

The screen, without setting it up. The firmware lit one output on each board (UEFI's GOP on the CIX, U-Boot on the RV2), and `display` (`drv/display`) takes that over: the mode, the timing and the link stay as the firmware left them. Mode setting, EDID and a second monitor come later. What display adds is **planes**: buffers the controller shows over one another and composes as it scans out, each moved or flipped at a vblank without copying a pixel ([docs §16.3](../../docs/16-rtg-display-gpu.md), pillars 3 and 4). `intuition` will be its client (V6). Now the test is (`conformance/displaytest.c`).

It drives:
- **the K1's HDMI DPU** (the RV2), "saturn" (`saturn.c`);
- **the CIX's DPU4**, Arm China's Linlon-D60, komeda's D71 family (`komeda.c`);
- **the framebuffer alone** (`fb.c`), where there's no controller it knows, or it isn't lit: QEMU's. It composes the planes itself, at 60 ticks a second.

## Concepts

- **Two ports.** "con.0" is the text console ([chapter 3](03-console.md)). "display.0" is the planes, with `struct DisplayMsg` messages (`planar/devices/display.h`). Both are served by one process, on one output.
- **Planes** stack by number, 0 at the bottom. Plane 0 is the console's: the firmware's framebuffer, as the firmware scans it out (`DISPLAYP_CONSOLE`), and no client's. Above it are an overlay (1) and the cursor (2, `DISPLAYP_CURSOR`, on top). Each takes `DISPLAY_XRGB8888` (opaque) or `DISPLAY_ARGB8888` (blended over what's below, not premultiplied), up to the screen's size.
- **Buffers are the server's.** `DISPLAY_ALLOC` makes one where the controller can reach it (a DMA Area of display's lease: below the K1's 2 GiB, uncached), and maps it into the client, readable and writable. Its lines are 64-byte aligned (`dm_Stride`). A client draws in it; it never has to clean a cache.
- **A commit** sets some planes at once (`dm_Set`, `dm_Count` of them): each a buffer (or 0, off) and where its top left goes on the screen. Part of a plane may be off the screen, and is clipped. The server answers at once with the commit's number (`dm_Seq`), and the controller takes it at the next latch, the start of a frame. Commits made before that latch go together.
- **A plane is the client's** that first commits a buffer to it, until it commits it off, or goes.
- **The sync page** (`dm_Sync`, `struct DisplaySync`), one page per output, mapped read-only into each client: `ds_Vblank` counts vblanks, at `ds_Time` (`CurrentTime`'s); `ds_Disp` is the last commit on the screen; `ds_Safe` the last whose predecessors' buffers the controller no longer reads. On these controllers they move together. Each is woken with `WakeAddr`: wait with `WaitAddr`.
- **When a client goes**, dead or not, its planes go off at the next latch, and its buffers are freed after it: never while the controller may still read them ([docs §16.6](../../docs/16-rtg-display-gpu.md)). `DISPLAY_FREE` frees one the same way; a buffer a plane shows can't be freed (`ERR_BUSY`).
- **When display goes**, its controller goes on scanning out. hwdb's stop method turns every plane off but the console's and waits for the latch, so nothing goes dark. Until then the kernel holds the planes' buffers (they're DMA Areas a client maps: [AllocDmaArea](../autodocs/lease.md#leaseallocdmaarea)'s notes). A client keeps its mapping, and may go on drawing. The restarted display shows none of them: the client connects again, and allocates and commits again.

## Using it

From `conformance/displaytest.c`, shortened:
```c
struct MsgPort mp;
struct DisplayMsg *m;
const struct DisplaySync *sync;
uint64_t seq, v;

ConnectPort(&mp, "display.0", 20, 4, 2 * sizeof *m);
m = AllocMsg(&mp, sizeof *m);
memset(m, 0, sizeof *m);
m->dm_Msg.mn_Length = sizeof *m;
m->dm_Task = FindTask(nullptr);         /* every message: where to map */
m->dm_Op = DISPLAY_INFO;
DoMsg(&mp, &m->dm_Msg);                 /* dm_Width, dm_Height, dm_Plane[], dm_Sync */
sync = (const struct DisplaySync *)m->dm_Sync;

m->dm_Op = DISPLAY_ALLOC;
m->dm_Width = 256, m->dm_Height = 128, m->dm_Format = DISPLAY_XRGB8888;
DoMsg(&mp, &m->dm_Msg);                 /* dm_Buffer, dm_Address, dm_Stride */
/* ... draw at dm_Address ... */

m->dm_Op = DISPLAY_COMMIT;
m->dm_Count = 1;
m->dm_Set[0] = (struct DisplayPlane){ .dp_Plane = 1, .dp_Buffer = id, .dp_X = 40, .dp_Y = 40 };
DoMsg(&mp, &m->dm_Msg);
seq = m->dm_Seq;
while ((v = sync->ds_Disp) < seq)
    WaitAddr(&sync->ds_Disp, v, 0);     /* on the screen */
```

To flip, draw the next frame in a second buffer and commit it to the same plane. Once `ds_Safe` reaches that commit, the first buffer is free to draw in again.

## Rules

- Draw only in a buffer no commit on the screen shows: wait for `ds_Safe` before reusing the one you flipped away from.
- Plane 0 is the console's. A commit to it is refused (`ERR_RIGHTS`); one to a plane another client holds, `ERR_BUSY`.
- Every message carries the client's task (`dm_Task`): the server maps the sync page and buffers into it.
- After display dies, `DoMsg` says `ERR_PORT_DIED`. Connect again, and allocate again: the old buffers are still yours to write, but nothing shows them, and they're freed when you end.

## The machines

**The K1** (`saturn.c`, `saturn.h`; the vendor kernel's `drivers/gpu/drm/ky` and omega's `plat/ky-x1`). The DPU node in U-Boot's tree has two windows, DSI and HDMI; display drives the HDMI one, `0xc0440000`, on its online pipeline 2. U-Boot scans out on RDMA 3 in composer slot 0, the top: display moves that slot to the bottom (7). The overlay is RDMA 2 in slot 6, the cursor RDMA 0 in slot 0. RDMAs 2 and 3 share a line buffer, which U-Boot leaves overlapping. display splits it, one line of the screen to RDMA 2 and the rest to RDMA 3. A commit writes the RDMAs and slots, then `CTL2_CFG_RDY`, which the hardware clears as it latches at the start of the frame. The vblank is the vsync interrupt, PLIC line 139, which U-Boot's tree doesn't list (hwdb's quirk). The DPU's MMU stays off, and the clocks, resets and timing are never touched.

**The CIX** (`komeda.c`, `komeda.h`; komeda's `d71`, FreeBSD's `drm-komeda-kmod`, the vendor's `linlon-dp`). Registers in 0x200-byte blocks, which display walks to find its compiz, timing controller and layers. The GOP's layer (or layers, and a scaler path, if it chose to scale) feeds the lowest compiz inputs; display's planes are layers the GOP left off, each straight into an input above them. A commit writes them, then `GCU_CONFIG_VALID0`, which reads 0 again once latched. The vblank is the DOU's PL0 interrupt, line 356, at the line `BS_PROG_LINE` names: display sets it to the line before `BS_PREFETCH_LINE`, as the vendor's driver does, since the GOP's line 0 comes before the commit is taken. The DPU isn't coherent (`_CCA` 0), and **it is never reset**: a reset drops the DisplayPort link through the PS185 for good. Of the five DPUs only DPU4 is lit, and display is kept off the rest.

**QEMU** (`fb.c`): edk2's GOP on `ramfb`. The console draws in a copy of the screen, and the screen is that copy with the planes over it, written out where something changed, at 60 frames a second.

**Flips, measured** by `displaytest`: 60 flips, each committed just after a vblank, timed by omega's method from the latching vblank's `ds_Time`.
- **The RV2** (2026-10-22, 59.9 Hz): commit to latch 16.63–16.65 ms, vblank to latch 16.65–16.67 ms. Every flip latched at the next vblank: none missed.
- **The CIX** (2026-10-22, 59.9 Hz): commit to latch 16.66 ms, vblank to latch 16.67 ms, none missed. That's with PL0 moved: the GOP leaves it at line 0, before the prefetch line (4) where the DPU takes a commit, so a commit made at the vblank was a frame late (33.3 ms) until display moved it to line 3.
- **QEMU** (TCG): none missed, at 16.6–24.9 ms, as its 60 Hz ticks come late.

## Functions used

- [ConnectPort](../autodocs/msg.md#execconnectport), [DoMsg](../autodocs/msg.md#execdomsg), [WaitAddr](../autodocs/exec.md#execwaitaddr): the client's side.
- [AllocDmaArea](../autodocs/lease.md#leaseallocdmaarea), [ShareArea](../autodocs/exec.md#execsharearea), [UnshareArea](../autodocs/exec.md#execunsharearea), [WakeAddr](../autodocs/exec.md#execwakeaddr): the server's.
- [drv_publish](../autodocs/drv.md#drvdrv_publish): two ports on one device.
