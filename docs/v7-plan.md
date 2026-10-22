# V7 plan: Workbench I, windows

**Status: in progress (2026-10-22); decisions 1 to 7 taken as they leaned.** Scoped from the [roadmap's V7 row](08-roadmap.md) and its decision on who owns a window's buffers, §13.1 and §13.3 (the layer split, zero-copy surfaces, crash containment, the window's chrome), and §16.3's pillars 1, 3 and 4 (`BitMap`, planes, presentation). The decisions at the end are taken.

**Goal (§8):** windows on the screen of both boards, composed on the CPU, with the pointer on its own plane. A client draws into its own memory with `gfx.library`, presents it without a copy across IPC, and is dragged, raised, closed and, when it dies, cleared away by `intuition`, which the client can't hang. Text is the 8×8 font until V8.

**What's already built.**
- **display** (V5 step 3) takes over the mode the firmware left: 1920×1080 at 60 Hz on both boards, and the GOP's mode on QEMU's `ramfb`. It has three planes on every backend: 0 the console's, 1 an overlay, 2 the cursor. Neither DPU has a cursor in hardware, so plane 2 is an ordinary layer on top.
  - `DISPLAY_ALLOC` makes a buffer as one of display's DMA Areas: NC on the boards, cached on `fb.c`. It's shared read-write into the client.
  - `DISPLAY_COMMIT` sets planes all at once, and commits between two latches go together.
  - The sync page's `ds_Disp` and `ds_Safe` are woken at the latch. On both boards a commit made after a vblank latches at the next one, with none missed in 60.
  - A client's planes go off at the next latch when it goes. When display goes, a client's buffers are held until hwdb's stop method has turned the planes off.
  - There's no import: every buffer the screen shows is display's.
- **input** (V5 step 4) gives USB HID boot-protocol keyboards and mice, through xhci, as 32-byte `InputEvent`s in a one-page ring.
  - Pointer motion is relative only, with no wheel. There are 3 buttons, and key repeat is done in software.
  - **One client holds the ring;** the rest get `ERR_BUSY`. Today that's the Shell, for keys.
  - hwdb restarts input 10 ms after it ends. The ring's holder opens it again.
- **The 8×8 font** is `drv/display/font8x8.c`, private to display's console.
- **The only compositor** is `fb.c`'s: QEMU's planes blended over the console's copy by the CPU, 60 times a second.
- **Shared libraries** (V6 step 4): `libs/<name>`, PIE images of code and constants alone, with no `.data` or `.bss`. They're reached through interface tables, with SIMD chosen through the table at boot. exec.library is 8.6 KiB on RV64.
- **Supervision** (V6 step 7): `OpenPort`'s wait, deadlines on calls to core servers, and hwdb keeping display and input. The kill test's `T_DISPLAY` and `T_INPUT` phases run `displaytest` and `inputtest`.
- **User-mode cache maintenance** on both ISAs: `dc cvac` (`SCTLR_EL1.UCI`) and Zicbom's `cbo.clean` (`senvcfg.CBCFE`).

**Not built, and needed here:**
- A screen on RV64 QEMU. Only `EFI=1` on AArch64 has one (edk2's GOP on `ramfb`). So none of V7's tests would run on RVV before the RV2.
- `gfx.library`, `BitMap` and `RastPort`: no drawing code at all, outside display's console and `fb.c`.
- `intuition`, its client library, and a way for input's events to reach more than one program.

**Exit (§8, §13.6):**
- Windows are opened, dragged by their title bars, and depth-arranged on both boards. intuition draws the chrome, and drags a window whose client has stopped answering.
- `PresentWindow` takes a window's buffer without copying it across IPC. A client presenting every frame gets 60 Hz, with none missed in 600 frames, on both boards. Its back buffer is free again by `safe_seq`, with no kernel call when it's already free.
- `kill-window`: a killed client's windows are gone at the next latch. Its Areas are unshared, and intuition's handle and page counts are back where they were.
- intuition, display and input are each killed under a running client. The client reconnects without polling and opens its windows again. The screen never goes dark: the console is under intuition's plane.
- `gfx.library`'s rectangles, blits and bevels pass a host test against a scalar reference, bit-exact, on RVV and NEON.
- The kill test passes 10 runs in a row on each board, with its new phases.
- The manuals are current (roadmap rule):
  - *Libraries* chapters on `gfx.library` and `intuition`;
  - `gfx.md` and `intuition.md` in the autodocs;
  - the display and input chapters for what changed.

Each step ends with `make test-all` green and a `V7.n done` line in the transcript.

| Step | Content |
|---|---|
| **1. A screen everywhere** | RV64 QEMU's `ramfb` set up by Planar through `fw_cfg`; `fb.c` on it; a screen's checksum for tests |
| **2. `gfx.library`** | `BitMap` over Areas (`AllocBitMap`, `FreeBitMap`, `GetBitMapAttr`); `RastPort`; `RectFill`, `BltBitMap` (`SRC`, `SRC_OVER`), bevels, 8×8 text; scalar and SIMD through the table |
| **3. intuition I: the screen and presentation** | The server, the composed screen on plane 1 and the pointer on plane 2; `OpenWindow`, `CloseWindow`, `PresentWindow` with `disp_seq` and `safe_seq`; damage; intuition's client library |
| **4. intuition II: input and the chrome** | intuition holds input's ring; focus; IDCMP events on a ring per connection; title bar, close and depth gadgets; dragging and depth-arranging; a hung client |
| **5. Supervision** | `kill-window`; intuition, display and input killed under a client; hwdb keeping intuition; the kill test's `T_WINDOW` phase |
| **6. The boards** | 60 Hz presentation measured on both; composition's costs; the kill test 10 runs in a row on each, transcripts kept |

Steps 1 and 2 are independent. Step 3 needs both, and step 4 needs 3. Step 5 needs 4, and step 6 needs 5. Nothing before step 6 needs a board, and step 6 needs both, one at a time.

## 1. A screen everywhere

QEMU's `ramfb` is a framebuffer in guest memory that the guest describes through `fw_cfg` (`etc/ramfb`: address, format, width, height, stride). On AArch64 edk2 does that, and Planar inherits a GOP. On RV64 nothing does, so Planar does it itself.

- **The kernel** finds `fw_cfg` (`qemu,fw-cfg-mmio` in QEMU's tree), as `plat/qemu-virt`'s `plat_framebuffer`, the hook the K1 finds U-Boot's framebuffer by. It writes `etc/ramfb` by fw_cfg DMA for 1280×800 XRGB8888, under the device tree at the top of RAM's first bank. From then on it's the board's framebuffer like the K1's: kept from the allocator by `memmap.c`, given to hwdb in `FirmwareInfo`, and leased to display, which binds `fb.c` to it unchanged. *Built so, not in hwdb as first planned:* a lease may cover only the firmware's framebuffer, and the kernel's is the one place that already knows it.
- `mk/qemu-virt-rv64.mk` adds `-device ramfb`, and `make test-all`'s RV64 boots get a screen.
- **A screen's checksum** for the tests: `DISPLAY_INFO` grows nothing. A test reads the composed screen back through a new `DISPLAY_READ` (fb.c alone: the boards' DPUs scan out of memory the CPU can't cheaply read). It answers with a CRC-32 of a rectangle. Steps 2 to 5 check pixels this way, against the host's reference.

**Tested by:** `displaytest` running on RV64 QEMU as on AArch64's EFI boots, and reading its last flip's overlay back with `DISPLAY_READ`, against a CRC-32 of what it drew. `V7.1 screen ok` gives the size and the backend, on every QEMU boot with a screen.

**Done (2026-10-22).** RV64 QEMU boots with a 1280×800 screen, and displaytest's flips, vblank pacing (none missed in 60) and display's death all run there. `BOOT_TIMEOUT` went from 20 to 40 s: RV64's boot now takes about 20.

## 2. `gfx.library`

§16.3's pillar 1 and §16.4's surface operations, CPU only (`GFX_BACKEND_SIMD`). Vector paths and glyph runs are V8's.

- **`struct BitMap`** as §16.3 has it: an Area handle, the base address, width, height, `bm_BytesPerRow` (a multiple of 64), `bm_PixelFormat` (`PIXFMT_XRGB8888`, `PIXFMT_ARGB8888`), `bm_Modifier` (`MOD_LINEAR` only), `bm_Flags`, `bm_ColorSpace` (`CS_SRGB`).
- **`AllocBitMap(w, h, format, flags, friend)`** makes an Area of the caller's: `BMF_CLEAR` and `BMF_DISPLAYABLE` are accepted. `BMF_DISPLAYABLE` with a `friend` takes the friend's stride and cache attribute (decision 1). `FreeBitMap` frees it, and `GetBitMapAttr` answers V39's queries.
- **`struct RastPort`**, caller-owned: the bitmap, a clip rectangle, the pens, the draw mode and the font. The library keeps no state (V6's rule).
- **The operations:**
  - `RectFill`, solid;
  - `BltBitMap` with `BLEND_SRC` and `BLEND_SRC_OVER` (ARGB, not premultiplied, as display's formats are);
  - `DrawBevel` (§13.2's light from the top left: a 1 px highlight and a 1 px shadow, from two pens);
  - `Text` in the 8×8 font, moved into the library from `drv/display/font8x8.c`. display links the same table.
  - Each one clips to the rastport.
- **SIMD through the table** (V6 decision 4): each operation has a scalar entry and an RVV or NEON one. The kernel repoints the table at boot by `arch_has_vector()`, as it does exec's `CopyMem`.

**Tested by:**
- **`make gfx-test` on the host**, with the sanitizers. It runs every operation at every width from 0 to 130 and 8 alignments, clipped on each edge, between guard bytes. The SIMD entries are checked against the scalar ones, bit-exact. They run under QEMU's user mode (`qemu-riscv64`, `qemu-aarch64`) where the host can't run them itself. It's part of `make test-all`. Mutation-tested.
- **`conformance/gfx.c`** on every QEMU boot: the same cases through `IGfx`, a bitmap shared with a second process, and the screen's checksum of a test pattern blitted to plane 1.
- **`V7.2 gfx ok`**, with the costs.

**Done (2026-10-22).** `include/planar/gfx.h`, `lib/gfx`, `libs/gfx` and `arch/<isa>/lib/gfx.c`: gfx.library 7.2, 6.5 KiB on RV64, the region's third library. `make gfx-test` passes 32,184 cases through the scalar table and through NEON's. In the guest, `conformance/gfx.c` passes the same cases through `IGfx` by RVV and by NEON. displaytest's gfx.library overlay is read back from the screen as the reference draws it (hwdb's `V7.2 screen ok`). Built so, not quite as planned:
- **RVV is checked in the guest, not under `qemu-riscv64`.** QEMU's user mode is Linux's, and the host is macOS. The host test runs NEON natively, as the host is an Arm one. RVV's entries are checked by `conformance/gfx.c` on both RV64 boots, against the same cases (`conformance/gfxcheck.h`). Its reference (`conformance/gfxref.h`) shares nothing with lib/gfx: a pixel at a time, rounding its own way.
- **`InitBitMap`**, which the plan didn't list, describes memory a program already has: a display buffer, and intuition's scanout buffers in step 3. `FreeBitMap` leaves such a bitmap alone.
- **An allocated `BitMap` lives in its own Area**, after the last row. Sharing `bm_Area` shares the description, and there's nothing else to free.
- **`Text` has no vector entry.** At 8 pixels to a row there's nothing for a vector unit to do. `RectFill`, `BltBitMap` and `DrawBevel` have one each, through one set of row kernels (fill, copy, blend) under one clipping driver (`lib/gfx/draw.c`).
- **`BLEND_SRC_OVER` takes the destination as opaque**, blending alpha as a channel whose source is 255. Each channel is rounded to the nearest, exactly (`lib/gfx/rows.h`), which the vector kernels compute to the bit. Translucent destinations wait for V8 (VQ33).
- **The screen's checksum is displaytest's**, not `conformance/gfx.c`'s: only hwdb's display test has a display to read.
- **Mutation-tested**, on the host and in the guest. Each of these failed a check:
  - the blend's rounding, in each of the three kernels;
  - NEON's alpha weights;
  - a vector kernel's tail left undone;
  - each edge of the clip, and the source's edges;
  - the order of rows for an overlap, and the cutting of a row for one;
  - `JAM1`'s background;
  - a glyph skipped at the left edge;
  - a friend's stride and placement;
  - the light drawn over the dark;
  - RVV's blit copy reading one pixel off.
  
  Three mutants survived, and all were equivalent: a shorter `vl`, NEON's fill handing its eighth pixel to the tail loop, and the two dark edges in the other order.
- **Costs under TCG** (a megapixel). On RV64, `RectFill` takes 1.07 ms scalar and 1.18 ms by RVV, and `SRC_OVER` 3.8 ms against 9.0 ms. On AArch64, `RectFill` takes 1.50 ms scalar and 0.37 ms by NEON, and `SRC_OVER` 3.9 ms against 5.9 ms. TCG runs most vector instructions an element at a time, so these say nothing of the hardware: step 6 measures the boards.

## 3. intuition I: the screen and presentation

`servers/intuition`, a server hwdb keeps, as it keeps input. One screen (the Workbench's), on one output.

- **The screen** is composed by the CPU into two full-screen buffers from `DISPLAY_ALLOC`, shown on plane 1 in turn (VQ31's leaning; decision 2). The console stays on plane 0 beneath it.
  - The backdrop is a flat `bg.backdrop.base` for now.
  - Composition is driven by damage. Each frame repaints the union of this frame's damage and the last frame's, since the back buffer is a frame old.
  - Windows are opaque in V7, so composition writes the scanout buffer and never reads it (decision 3).
- **The pointer** is a 16×16 ARGB sprite (§13.2's `cursor.fill`) on plane 2, moved by a commit of plane 2 alone. Moving it dirties no window.
- **Windows** are client-owned (decision 1). A client makes a `BitMap` or two and opens a window with them. intuition maps each one read-only by `ShareArea` and composes from it. It never writes a client's memory.
- **The protocol**, on the `intuition` port:
  - `INTUI_OPEN`: the window's size, position, title and flags, and its buffers' Area handles. It answers with the window's number and its sync words.
  - `INTUI_CLOSE`.
  - `INTUI_PRESENT`: a buffer's index, up to 16 damage rectangles, and the frame's sequence number.
  - Every message has a deadline (V6 step 7).
- **The sync words** (§16.3 pillar 4), on a page shared read-only with the client, one cache line per window:
  - `disp_seq`: the frame whose pixels are latched, with the latching vblank's time.
  - `safe_seq`: the last frame intuition has finished reading. With CPU composition, `safe_seq` moves when the copy is done, before the latch, not after it as display's `ds_Safe` does. A client checks it and goes on with no kernel call if its back buffer is free. It calls `WaitAddr` only when it's ahead.
- **intuition's client side** (decision 5): `OpenWindowTags`, `CloseWindow`, `PresentWindow`, `WaitSafe`, built on `ConnectPort` and the message library.

**Tested by:**
- **`make intuition-test` on the host:** composition and damage over made-up windows and a fake display, checked pixel for pixel against a naive compositor that redraws everything. It covers windows overlapping, off the screen's edges, and moved between two frames.
- **`conformance/window.c`**, on every QEMU boot with a screen:
  - two windows opened, presented and checked by the screen's checksum;
  - 120 frames presented as fast as `safe_seq` allows, counting the kernel calls a frame and the frames missed;
  - the refusals: a buffer too small, a buffer not shared, a damage rectangle outside the window, a window that isn't the caller's.
- **`V7.3 windows ok`**, with the costs: composition per megapixel, present to latch, calls per frame.

## 4. intuition II: input and the chrome

- **The keyboard and pointer.** intuition holds input's ring. The pointer's position is intuition's: relative motion, clamped to the screen, with no acceleration in V7. How the console Shell gets keys while intuition runs is decision 4.
- **Focus.** A button press over a window activates it and brings it to the front. Keys go to the active window.
- **IDCMP**, Intuition's names, on a ring per connection that intuition writes and the client reads, as input's is:
  - `IDCMP_MOUSEBUTTONS` and `IDCMP_MOUSEMOVE`, in the window's coordinates;
  - `IDCMP_RAWKEY` (the HID usage and qualifiers) and `IDCMP_VANILLAKEY`;
  - `IDCMP_ACTIVEWINDOW` and `IDCMP_INACTIVEWINDOW`;
  - `IDCMP_CLOSEWINDOW`.

  A client asks for the classes it wants. A full ring drops the oldest moves first, and never a button or a key.
- **The chrome** (§13.3), drawn by intuition with `gfx.library`:
  - a 26 px title bar with the title in the 8×8 font;
  - the depth and close gadgets;
  - a 4 px bevelled frame;
  - active and inactive colours.

  The window's buffer is its inside, and the chrome is intuition's. Iconify, zoom, the border scrollers and resizing are V8's, with `ui.library`.
- **Dragging** the title bar moves the window, recomposing only the area it left and the area it now covers. **The depth gadget** sends a window to the back, or brings it to the front if it's already at the back.
- **A hung client.** None of this waits on the client. Its window is dragged, raised and lowered on its last presented frame. A close gadget sends `IDCMP_CLOSEWINDOW`. If the window isn't closed within the deadline, what intuition does is decision 6.

**Tested by:**
- **`conformance/window.c`'s input half**, on QEMU through QMP (`input-send-event`, relative moves and buttons, as `inputtest` types keys):
  - a window dragged, and checked moved by the checksum;
  - two depth-arranged;
  - focus moved by a click;
  - keys arriving only at the active window;
  - `IDCMP_CLOSEWINDOW` on the close gadget.
- **A client that stops answering** (a mode of `window.c`) is dragged and raised while it sleeps.
- **`V7.4 input ok`.**

## 5. Supervision

- **`kill-window`** (§13.1's crash containment). A client with two windows is killed. intuition sees `CONNF_CLIENT_GONE`, unlinks the windows, unshares their Areas and damages where they were. The test checks:
  - the screen by its checksum at the next latch;
  - intuition's handle count (`QueryProcess`) and page count back where they were;
  - the client's Areas freed.
- **intuition killed** under a client. The console shows through at the next latch, after hwdb's stop method turns planes 1 and 2 off as display's does now. The client's next call returns `ERR_PORT_DIED`. It waits for the port (`OpenPort`'s wait), opens its windows again from its own buffers, and presents. Nothing of the client's is lost, since its buffers were always its own.
- **display killed** under intuition. intuition reconnects, allocates its two buffers again and composes everything. Its clients see one frame's `disp_seq` late, and nothing else.
- **input killed.** intuition opens the ring again. The pointer stays where it was.
- **hwdb keeps intuition** as it keeps input. `T_WINDOW` in the kill test runs all of the above, and `V7.5 done` closes it.

**Tested by** the kill test, on every QEMU boot with a screen, mutation-tested: an intuition that forgets to unshare fails `kill-window`'s page count.

## 6. The boards

On the CIX, then the RV2:
- **`window.c`'s 600 frames** at 60 Hz, a full-screen window and a small one, with `disp_seq`'s latch times by omega's method, as `displaytest` measures them now. None may be missed.
- **Composition's costs:** a full screen, a 640×480 window dragged, and the pointer alone. Into NC on both boards, and into cached memory with the damage cleaned, to settle decision 3 on the hardware.
- **`gfx-test`'s SIMD entries on the hardware:** the X60's 256-bit vectors and the A720's NEON, against the scalar ones, as V6.1's costs weren't yet measured on either board.
- **The kill test 10 runs in a row on each**, with `T_WINDOW`. The transcripts go in `docs/boards/*/v7-kill-test/`.
- **What to look at by eye**, once on each board: windows dragged by a USB mouse, the pointer smooth, no tearing. The checksum can't see the DPU's own output.

## Sources

- §13.1 (layer split, zero-copy surfaces, crash containment), §13.2 (colours), §13.3 (chrome), §13.6, §13.7 (VQ31, VQ33), §16.3 (pillars 1, 3, 4), §16.4, §16.6, §16.9 (VQ38, VQ40), §12.
- The RKM's *Libraries* (3rd edition): Intuition's windows, IDCMP and screens, and graphics.library's `BitMap`, `RastPort` and blitter calls, for the names.
- QEMU's `docs/specs/fw_cfg.rst` and `hw/display/ramfb.c` for step 1.

## Decisions

1. **Who owns a window's buffers.** The roadmap's leaning was the client, through `AllocBitMap` with the display as its friend, so that the DMA limit and the cache attribute come from the device. But with the CPU composing (decision 2), nothing scans out a client's buffer. intuition only reads it, so a window's buffer needs nothing from the device. *Leaning:* the client, as plain cached Areas in V7, with no import op in display. `BMF_DISPLAYABLE` with a friend comes when a window is promoted to a plane of its own (VQ31's second half, after V7). It will need display to take a client's Area under its lease's limit and quarantine, `area_hold_lent`'s other direction.
2. **What intuition composes into.** Plane 1, the overlay, over the console. Or plane 0, which takes the console off the screen. *Leaning:* plane 1. The console under it is the screen when intuition is gone, and display needs no change. It costs one plane, and the boards have only one overlay today. So promotion (VQ31's second half) needs more of the DPU's layers first: the K1 has 4 RDMAs, and the CIX's pipeline has 4 layers.
3. **The scanout buffers' memory.** NC (write-combined), composed write-only. Or cached, with the damage cleaned at each frame (`dc cvac`, `cbo.clean`). *Leaning:* NC, as display allocates now: opaque windows never need to read the screen. Step 6 measures both, and the choice is revisited for V8's translucency (VQ33).
4. **The keyboard while intuition runs.** input has one ring holder, and today it's the console Shell. Until V8's windowed Shell, there are three ways to share it:
   - intuition starts only on demand (a boot argument, the kill test, a command), and the Shell keeps the keyboard otherwise;
   - input serves two holders, with a focus word saying which one gets keys;
   - intuition passes keys to the Shell while no window is active.

   *Leaning:* on demand. The board's default boot stays the console Shell with USB keys, and intuition's own tests take the ring. V8 makes intuition the default with the Shell in a window.
5. **intuition's client side: a shared library or a static one.** `intuition.library` in the shared region, as the roadmap's V6 row has it, or `lib/intuition` linked in. *Leaning:* shared. Its state is the caller's `Window` structures, which V6's rule allows, and V8's `ui.library` will call it through `IIntuition`.
6. **A window that isn't closed.** When the close gadget's `IDCMP_CLOSEWINDOW` gets no `CloseWindow` within the deadline: remove the window and say so in the log, or kill the client. §13.1 says intuition *offers* to `KillProcess` it. intuition has no rights over another process, and V7 has no requesters to ask with. *Leaning:* remove the window and log it. Killing waits for V8's requesters and a right to ask hwdb for the kill.
7. **RV64 QEMU's screen.** Set up `ramfb` from Planar (step 1), or test windows on AArch64 QEMU and the RV2 alone. *Leaning:* set it up. It's about 100 lines (in the kernel's QEMU plat, as built), and it puts every RVV path of step 2 and every intuition test on half of `make test-all`'s boots.
