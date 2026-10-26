# V7 plan: Workbench I, windows

**Status: in progress (2026-10-22); decisions 1 to 7 taken as they leaned; steps 1 to 5 done; step 6 done on the RV2, and on the CIX all but its 10-run kill test, which the board's own resets keep stopping (see [step 6's *To continue*](#to-continue)).** Scoped from the [roadmap's V7 row](08-roadmap.md) and its decision on who owns a window's buffers, §13.1 and §13.3 (the layer split, zero-copy surfaces, crash containment, the window's chrome), and §16.3's pillars 1, 3 and 4 (`BitMap`, planes, presentation). The decisions at the end are taken.

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

**Done (2026-10-22).** `servers/intuition` (the server and its compositor, `compose.c`), `include/planar/intuition.h`, `lib/intuition` and `libs/intuition`: intuition.library 7.3, 6.6 KiB on RV64, the region's fourth library. `make intuition-test` passes 10,865 frames of random windows against the naive compositor, through the scalar table and NEON's, with no pixel written twice in a frame. `conformance/window.c` passes on the six QEMU boots with a screen, run by hwdb's new `T_WINDOW` phase after `T_DISPLAY`. hwdb then kills intuition and checks display freed its three buffers. Built so, not quite as planned:
- **hwdb doesn't keep intuition yet.** It starts intuition for the window test (decision 4: on demand), and kills it after. Keeping it and restarting it is step 5's.
- **A buffer is sent by its address, not its Area's handle.** Handles are the process's own, and mean nothing to intuition. intuition finds the Area with `QueryArea`: it must be another process's (`ai_Area` 0, two sharers at least), readable, and hold every row the buffer says it has. intuition describes the buffer itself with `InitBitMap`, and never reads the client's `BitMap`.
- **`INTUI_INFO`**, which the plan didn't list, gives the client intuition's task, to `ShareArea` its bitmaps with before `INTUI_OPEN`, and the screen's size. It gives the tests intuition's costs too.
- **A window joins the screen at its first frame**, at the front, so nothing half drawn is shown. Then the window's latest frame is taken whenever intuition composes. A frame that changes nothing on the screen (a window off it) is safe and shown at once.
- **The sync words are `is_Safe`, `is_Disp` and `is_DispTime`.** `safe_seq` became `is_Safe`: the last frame composed, after which intuition reads nothing of the window's other buffer. So `WaitSafe` waits for `is_Safe` to reach the frame last presented, with two buffers or one.
- **`PresentWindow` doesn't wait for its answer.** It sends on one of 4 messages of its own, whose answers are taken later. A refusal of intuition's comes back with the next `PresentWindow` or `WaitSafe`. The library makes intuition's checks first, so such a refusal is a client's bug.
- **intuition waits for the latch with a second task**, in `WaitAddr` on display's `ds_Disp`, which signals the first. The first sleeps on its ports.
- **The pointer doesn't move yet.** It's committed on plane 2 with the first frame, at the screen's centre. Step 4 moves it, with input.
- **`pi_Calls`** was added to `QueryProcess`'s `ProcessInfo`: a process's own kernel calls (`process_calls`, the kernel's, until now for its self-tests). `window.c` counts a frame's with it, and checks that `WaitSafe` on a free buffer makes none.
- **hwdb's display connection** is made again when it's asked for, if display's restart was seen before its old connection's end (`display_ask`). The window test asks display for its buffer count after `T_DISPLAY` killed display.
- **What a client can still do to intuition:**
  - unshare a bitmap under an open window, which ends intuition at its next read (`UnshareArea`'s note), for step 5's supervision to restart;
  - give the address of its connection's arena as a buffer, which is the kernel's and shared with both, and is shown.

  Neither touches another client's memory.
- **Mutation-tested.** In the compositor, each of these failed `intuition-test`:
  - the last frame's damage left out;
  - a piece beside a window taken too tall, or one dropped;
  - damage merged without starting again;
  - a move not damaging where the window was;
  - a frame's damage overflowing, dropped.

  One survived, and it's equivalent: clipping a pixel past the screen's right edge, which gfx.library clips again. In the server and the library, each of these failed `window.c`:
  - damage one column past the window, accepted;
  - `is_Safe` never moved, or waited for a frame too far;
  - the Areas kept after `CloseWindow`;
  - the same screen buffer shown each frame;
  - a buffer's rows not checked against its Area;
  - a window's height not checked against its buffer's;
  - a frame off the screen never made safe;
  - `WaitSafe` making a kernel call when the buffer is free.
- **Costs under TCG.** Composing takes 2.6 to 3.5 ms a megapixel on RV64, and 1.9 to 2.0 on AArch64. A frame presented one at a time takes 14 to 21 ms to latch on RV64, and 10 to 18 on AArch64, a vblank or so. Presented as fast as `is_Safe` allows, 120 frames are all shown, in 120 vblanks (124 at worst, on RV64), at 3 kernel calls a frame: `WaitSafe`'s `WaitAddr`, the signal that wakes intuition, and `window.c`'s own `CurrentTime`. intuition's buffers are cached on QEMU (`fb.c`'s), so NC's cost is step 6's to measure.

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

**Done (2026-10-22).** intuition holds input's ring, moves the pointer, keeps focus, draws the chrome (`servers/intuition/chrome.c`) and drags and depth-arranges windows. A connection's events go on a ring in its page (`events.c`). intuition.library 7.4 adds `GetIntuiEvent` and `WaitIntuiEvent`, and the tags `WA_Flags` and `WA_IDCMP`: 7.2 KiB on RV64, and the shared region still 9 pages there and 13 on AArch64. `conformance/window.c`'s input half passes on the six QEMU boots with a screen, through the boot check's new `send move` and `send drag`. `make intuition-test` now composes windows with chrome among those without, tests what's where on the chrome and that nothing's drawn past its bitmaps, and runs 540,000 keys and buttons through `events.c` to a client reading slower than they come, at random: all came, in order. Built so, not quite as planned:
- **The ring's room is said by the client.** intuition never writes a client's memory, and a client can't write intuition's page, so the client shares a page of its own, read-only, saying how far it has read (`IntuiAck`, by a new op, `INTUI_EVENTS`). intuition writes an event only when there's room. It keeps up to 64 more, folding moves into the latest. Moves fill no more than 47 of the ring's 94, so keys and buttons are lost only when a client is 111 of them behind. A client that reads the ring down from full, or from 47, tells intuition with `SignalConn`. Otherwise reading an event is no kernel call.
- **One ring a connection**, not one a window: each event says its window. The RKM's `UserPort` and `ReplyMsg` have no counterpart.
- **The chrome is two bitmaps of intuition's for each window**, composed as four of its parts around the inside. A window in `compose.c` became up to five parts, each from its own bitmap. Each pixel is still written once.
- **`WA_Width` and `WA_Height` are the inside's**, with the chrome added around it. `WFLG_BORDERLESS` is step 3's window, all inside. The RKM's numbers for the tags, flags, IDCMP classes and button codes. A raw key going up is `RAWKEY_UP` (0x8000), as HID usages run past the RKM's 0x80.
- **Moves go to the active window** whenever it asked for them, with no `WFLG_REPORTMOUSE`. A button pressed over the inside goes to its window, and its release to the same window.
- **The pointer moving is a commit of plane 2 alone**, one a vblank at most, as a frame is.
- **Decision 6, as it leaned:** a close gadget unanswered for a second takes the window off the screen, logged. Its slot stays for the client's `CloseWindow`, its `is_Safe` goes to its highest so `WaitSafe` returns, and its next present is refused.
- **Where a window is** is in its sync words, `is_LeftEdge` and `is_TopEdge`.
- **If another program holds input's ring**, intuition runs without the pointer and keys. It doesn't take the ring again after input's death yet: that's step 5's.
- **A hole found, not closed:** intuition checks that a buffer is memory another process shared with it, not whose. So a client that knows the address of another's bitmap can show it. `QueryArea` would have to say an Area's owner. A client unsharing its `IntuiAck` ends intuition, as unsharing a bitmap does.
- **Not tested on the boots:** `GetIntuiEvent`'s `SignalConn`, as `window.c` never falls 47 events behind. `events.c`'s side of it is the host test's.
- **Mutation-tested.** In the server, each of these failed `window.c`:
  - focus never moved;
  - a press that didn't raise its window;
  - the depth gadget always lowering;
  - a drag not said in the sync words;
  - an unanswered close gadget never acted on;
  - mouse positions from the chrome's corner, not the inside's;
  - keys sent to the window under the pointer, not the active one. This one survived at first, as the pointer was over the active window whenever keys were typed. Now it's over the other.

  Moves let fill the whole ring failed `intuition-test`.

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

**Done (2026-10-22).** hwdb keeps intuition while it's wanted (`servers/hwdb/intuition.c`), and intuition outlives display and input. `T_WINDOW` is now two rounds of `conformance/window.c`. The first round ends with kill-window, after which hwdb checks intuition's handles and pages. The second runs across intuition's, display's and input's deaths, which hwdb causes. Both pass on the six QEMU boots with a screen, and `V7.5 done` closes them. intuition.library is 7.5. Built so, not quite as planned:
- **A kernel bug, found by kill-window.** `ReleaseArea` refused an Area whose owner had died or freed it, taking it for the kernel's (its handle 0). So intuition had never let go of a dead client's bitmaps, since step 3: they stayed mapped in intuition until it died. Nor could it have let go of a dead display's buffers. An Area now says whether a process allocated it (`user`), and a sharer may release one whose owner has gone. The kernel's own (arenas, stacks, images) are still refused. A dead owner's pointer is cleared too, since its record may be reused.
- **No stop method for intuition.** It has no device: display turns off a dead client's planes at the next latch, as it already did, and the console shows.
- **How intuition waits.** Two tasks of its own wait for display's and input's ports to come back, by `OpenPort`'s wait. Each closes the connection it made to find the port at once, and the main task connects itself, so the signals are its own. intuition goes on serving its clients meanwhile.
  - The tasks that watch `ds_Disp` and `ir_Head` are moved to the new words. The old ones are woken until each watcher has stopped reading them, so they can be let go of.
  - While display is away, a window of two buffers has its frame taken at once, as nothing reads the other buffer. A window of one waits for display.
  - Back, display gets the whole screen composed into two new buffers.
- **A client is told of intuition's death without polling**, by what the kernel already did: tasks waiting on a word in an Area are woken when a mapping of it goes. So `WaitSafe` and `WaitIntuiEvent` return `ERR_PORT_DIED`.
  - `CloseIntuition` now lets go of the connection's page itself, in case intuition is dead and can't take it back.
  - A `Window` of a closed connection is refused (`ERR_BADARG`), as its sync words were on the page that went.
- **hwdb's cues.** hwdb watches intuition with `INTUI_INFO` on a connection of its own. The second round opens a third window once its first two are checked: that's when hwdb kills intuition, and again, on intuition's next self, when it kills display. hwdb kills input once intuition has taken its buffers from the new display. `INTUI_INFO` gained `im_InputOpens`, so the client can see input's ring opened again.
- **"The screen never goes dark"** is checked as: where the windows were, the screen shows neither them nor the backdrop. The console's text isn't modelled.
- **kill-window's bitmaps.** The killed client shares its bitmaps with the first round too, so that round can watch them unshared and then freed. It shares them once that round has answered on a connection, as an Area is shared only with a peer.
- **When intuition starts, `ERR_BUSY` from input is tried again for a second.** A dead intuition's ring is let go of only when input sees the connection go.
- **The mouse after input's restart** comes back a little later, once xhci has told the new input of it. Moves sent before then are lost, so the client asks again.
- **Measured under TCG.** A killed client's windows read back gone within 1 to 3 vblanks of the kill, the reading back itself slow; the check allows 4, and step 6 measures the boards. 2 to 4 frames were presented while display was away, each safe at once.
- **Mutation-tested.** Each of these failed a boot:
  - intuition forgetting to let go of a dead client's bitmaps (the plan's), and the kernel's old `ReleaseArea`;
  - `CloseIntuition` keeping the page;
  - display's death ending intuition;
  - nothing taken while display is away;
  - the latch's watcher left on the old display;
  - the screen not composed again;
  - input's ring not opened again;
  - the pointer moved when input's back;
  - hwdb not starting intuition again.

  The last survived at first, as the client noted the pointer after hwdb might already have killed input. Now it notes it before display's death.
- **Not tested on the boots:** a one-buffer window while display is away, and display coming back with another size (intuition exits, for hwdb to restart it).

## 6. The boards

On the CIX, then the RV2:
- **`window.c`'s 600 frames** at 60 Hz, a full-screen window and a small one, with `disp_seq`'s latch times by omega's method, as `displaytest` measures them now. None may be missed.
- **Composition's costs:** a full screen, a 640×480 window dragged, and the pointer alone. Into NC on both boards, and into cached memory with the damage cleaned, to settle decision 3 on the hardware.
- **`gfx-test`'s SIMD entries on the hardware:** the X60's 256-bit vectors and the A720's NEON, against the scalar ones, as V6.1's costs weren't yet measured on either board.
- **The kill test 10 runs in a row on each**, with `T_WINDOW`. The transcripts go in `docs/boards/*/v7-kill-test/`.
- **What to look at by eye**, once on each board: windows dragged by a USB mouse, the pointer smooth, no tearing. The checksum can't see the DPU's own output.

**Built for it.**
- **`window.c`'s paced frames.** After its 120 frames as fast as `is_Safe` allows, it presents 600 frames of its 200×120 window, then 600 of a two-buffered window the screen's size, each presented whole just after a vblank. Each frame's latch is `is_DispTime`, the vblank's `ds_Time`. The test says present to latch, vblank to latch, and the vblanks missed, counted by the vblanks' period over the run, and fails on a board if any were. QEMU runs 30 of each and only says them. Its display composes the planes itself in software as it latches, the whole screen for intuition's plane, so RV64 QEMU misses one vblank a frame.
- **intuition's costs.** hwdb starts intuition with `INTUITION_COSTS` at the first start of a test, on a board's controller alone. Before publishing its port, intuition composes 60 frames of each case from a bitmap of the screen's size: the whole screen damaged, and a 640×480 window dragged 8 right and 4 down each frame. Each runs into display's NC screen buffers, then into two cached ones with each frame's damage cleaned (`drv_dma_clean` over its bounds). It then times the pointer alone, which composes nothing, as plane 2's commit, call to answer. The costs are said, not checked.
  - **`DISPLAYA_CACHED`**, a new `dm_Flags` bit (it was `dm_Pad`) for `DISPLAY_ALLOC`, gives a cached buffer where a controller would get an NC one. Its client cleans what it wrote before committing.
- **`look`**, a boot argument: the window test holds its first two windows on the screen for 20 s, and says on the serial line what should be there. The kernel passes it to hwdb as `HWDB_LOOK`, and hwdb to `window` as `LOOK`.

**The RV2 (2026-10-22, `15a6095`).** The kill test passed 10 boots in a row, each 36 kills (5–11 while binding), `T_WINDOW`'s two rounds, and every test to `V7.5 done`. The transcripts are in [`boards/orangepi-rv2/v7-kill-test`](boards/orangepi-rv2/v7-kill-test).
- **600 frames at 60 Hz, none missed**, in every run, of the small window and of the full-screen one. Each latched at the next vblank: present to latch 16.6–16.7 ms (one run's full-screen frames 16.2–17.2 ms), as displaytest's own flips (16.6 ms).
- **Composing**, in every run within 1%:

  | | NC | cached, cleaned |
  |---|---|---|
  | the whole screen (2.07 Mpx) | 4.29 ms | 4.12 ms |
  | a 640×480 window dragged (0.32 Mpx a frame) | 2.38 ms | 1.05 ms |
  | the pointer alone | nothing composed; its commit 14 µs (cut from the runs' log lines at `KPrintF`'s 160 bytes, measured at the next commit) | |

  Cached and cleaned is never slower. It's 2.3× faster for the drag, though why NC is so much slower there, at 7.4 ns a pixel against 2.1 for the whole screen, isn't measured. Decision 3 leaned NC. The RV2 says cached, and the CIX decides.
- **gfx.library on the X60's 256-bit vectors**, by the table against lib/gfx's scalar code, a megapixel: `RectFill` 529 µs against 1,283 (2.4×), `SRC_OVER` 6.8 ms against 29.8 (4.4×), `Text` 715 ns a glyph.
- **By eye**, with `look`: the backdrop and both windows where they should be. Before that, the windows had been missed at test speed, each scene up for a second or two, and the backdrop (#182038) taken for black.
- **Dragged by hand**, with `drag` (below): windows dragged by a USB mouse, and after the fixes below the pointer's feel "much better, and tracking acceptable". No tear was reported.

**The CIX (2026-10-22, `e085715`).** 13 boots passed every test to `V7.5 done`, each with 36 kills (3–9 while binding) and `T_WINDOW`'s two rounds. But no 10 in a row yet: both attempts were stopped by the board, not by a test (below).
- **600 frames at 60 Hz, none missed**, in every boot, of both windows: present to latch 16.65–16.67 ms, vblank to latch 16.66–16.68 ms, steadier than the RV2's. displaytest's flips latched one frame after their commit too (16.66 ms), where V5's notes say about two.
- **Composing**, NC faster here, the RV2's opposite:

  | | NC | cached, cleaned |
  |---|---|---|
  | the whole screen (2.07 Mpx) | 0.39–0.40 ms | 0.70–0.76 ms |
  | a 640×480 window dragged (0.32 Mpx a frame) | 0.085–0.098 ms | 0.19–0.21 ms |
  | the pointer alone | nothing composed; its commit 3–5 µs | |

  About 11× the RV2's speed composing into NC. **Decision 3 is split by the boards:** the RV2 composes faster into cached memory cleaned (2.3× for a drag), the CIX into NC (2.2×). Not yet decided: a per-board choice (display could say which, as `DISPLAYA_CACHED`'s caller) or NC everywhere, which costs the RV2 1.3 ms a drag frame of its 16.7.
- **gfx.library on the A720's NEON**, by the table against lib/gfx's scalar code, a megapixel: `RectFill` 84 µs against 546 (6.5×), `SRC_OVER` 1.08 ms against 5.96 (5.5×), `Text` 94 ns a glyph.
- **Dragged by hand** with `drag`: the pointer at first stayed at the top and moved only left and right. The CIX's mouse, a Corsair (`1b1c:1bac`), sends buttons and 16-bit X and Y even in the boot protocol, so input read X's high byte as Y. xhci now reads a mouse in the report protocol, through its report descriptor (`drv/xhci/hid.c`, `e085715`), and turns each report into a boot report. Then it tracked, "good": 4,941 moves 1–20 ms after the last, the pointer on the screen 31 ms after input had a move, on average. A drag still costs "drag me" frames (179 of 1,084 vblanks): a move of a window is composed at once, and a window's frame presented after it waits for the latch after. Pointer moves alone cost none (18,001 frames in 18,001 vblanks with the pointer moving in 992), from `POINTER_LEAD`.
- **The kill test's 10 in a row: not yet.** Attempt 1 passed 9, attempt 2 passed 4; the transcripts are in [`boards/cix-p1/v7-kill-test-attempts`](boards/cix-p1/v7-kill-test-attempts). Each was stopped by **the board's system reset hanging**: no TF-A line (`NOTICE:  BL2`) after the reset was asked for, the screen black, ssh gone, until the power was cycled.
  - Attempt 1, run 10: FreeBSD's own `reboot` (efi-boot's, over ssh) hung, with Planar last run a whole boot before, and its run 9 clean.
  - Attempt 2, run 5: Planar passed everything, said `planar: rebooting`, and its PSCI `SYSTEM_RESET` hung. The SBSA watchdog, left armed, didn't reset the board either.
  - What V7 leaves on the hardware at the reset is as V6 left it: the DPU's layers off but the console's (hwdb's `stop_display`), the USB controllers halted. New are the Corsair in the report protocol (the device's own state) and xhci's `IMOD` 32. Neither touches the start of a reset. V6's 10 runs were on the same bench, the same Corsair and hub plugged in.

#### To continue

1. **Find whether the CIX's reset hangs without Planar.** A loop of FreeBSD reboots over ssh (no Planar), say 20, counting hangs (no `NOTICE:  BL2` within 30 s on the serial line). FreeBSD alone hanging at a like rate means the method needs a way round a stuck reset (a power switch the host can work, or the watchdog left to the firmware), not a fix in V7. None means looking at what Planar leaves the SCP: its `power` talks SCMI to it on mailbox 6 and to TF-A on `0x84380000`, the channel a PSCI reset may use.
2. **Fix `power`'s SCMI busy window** (`drv/power/scmi.c`, `call`): it writes `SMT_STATUS` 0 (busy) before it rings the doorbell or makes the SMC, so a `power` killed between the two leaves the channel busy, perhaps across a warm reset, for whoever uses it next. The transcripts show it didn't happen in these runs (each run's later `power` answered on both channels), but V5.2 and V5.7 kill `power`, and V6's code has it too. A dead `power`'s channels could be made free again by hwdb, as `stop_display` turns a dead display's planes off: wait for the platform to free a rung channel, and free one that wasn't rung. **Done (host-tested, not yet on the CIX):** hwdb's `stop_power` (`servers/hwdb/power.c`) does that through `smt_release` (`drv/power/smt.h`, the SMT layout now shared with `scmi.c`): on SHM0, the SMC's channel, answered before its call returns, is freed at once if busy; mailbox 6's is given 300 ms (power's own wait) to be answered, then freed, and hwdb says which it freed. `make power-test` checks each case against the made-up agents. To see on the board: no such line in the 10 runs, or `power` answering after one.
3. **Then the CIX's 10 in a row**, `make PLATFORM=uefi-a64 test` in a loop as the RV2's ran, transcripts to `docs/boards/cix-p1/v7-kill-test/`, and the CIX's README row.
4. **On the RV2 again**: one `drag` boot, as its mouse (`30fa:0300`) now goes through the report-protocol path too (tested only by the host and QEMU's `usb-mouse`), and to measure `COMMIT_LEAD` (was `POINTER_LEAD`; item 6) there with a hand on the mouse.
5. **Decision 3**, from the two boards' costs above.
6. **Done in QEMU, to measure on the boards** (were left for after V7):
   - **A drag composed late in the frame.** Any frame no client waits for (the pointer moved, a window dragged, the chrome redrawn) is now put off as the pointer alone was: started `COMMIT_LEAD` (4 ms, `POINTER_LEAD` renamed) plus what composing it should take before the vblank (`ic_start` in `compose.c`). The cost is 200 µs plus the frame's pixels (`ic_pending`) at the worst rate of the last 8 frames of 64K pixels or more; with none yet, a frame is composed at once. `make intuition-test` checks `ic_start`'s cases and that `ic_pending` is what each random frame paints. To see on each board: a `drag` boot, against the CIX's 179 of 1,084 vblanks missed by "drag me" while dragging.
   - **The pointer's sprite twice the size** (32×32, each pixel 2×2) on a screen of 1600×900 or more (`pointer_scale` in `pointer.h`; window.c reads it back at the same scale). At the right and bottom edges only the tip shows, as on other desktops: the hot spot still reaches the last pixel. QEMU's screens (1280×800, 800×600) stay at 16×16.
7. **V7's exit**: the manuals' *Checked against* lines moved to V7, and the plan's status to done.

**The drag test** (`drag`, a boot argument as `look` is: `HWDB_DRAG`, `WINDOW_DRAG`). After its first two windows, the window test opens two with chrome, "drag me", 640×480 and two-buffered, and "close me to end", 320×200, until either's close gadget or 300 s. "drag me" presents a frame of moving stripes as soon as its last is on the screen, so a tear would show as a broken stripe. At each latch the test notes where each window is. After, it says the vblanks "drag me" missed, how far each window moved and how often it stood still for one frame between two that moved, and, from `INTUI_INFO`'s new `im_Moves`, `im_MoveNanos`, `im_MoveMax`, `im_ReadMax` and `im_MoveGaps`, the pointer's moves from input's `ie_Time` to the latch, and the gaps between them. It says every 30 s how it's going, since `serial-boot.py` gives up on 120 s without output. hwdb's and the kernel's waits for the window test grow by 300 s with it, and by 20 s with `look`. Said, not checked: a hand isn't a test.

**What the drag test found on the RV2:**
- **The mouse's reports came in bursts**, 4 at a time, 8 times a second: in one run, 2,241 moves within 1 ms of the last, 341 after 100 ms to 1 s, none between. It felt laggy, the pointer seeming to move against the hand when the hand turned. Each report was a well-formed boot report. **The DWC3's interrupt moderation counts at 32 kHz**: `IMOD` 4000, 1 ms in xHCI's 250 ns ticks, held its interrupts 125 ms apart (80 in every 10 s). xhci now writes 32: 1 ms there, 8 µs elsewhere. After, 4,491 moves came 1 to 20 ms after the last, none in bursts, about 100 a second, and the pointer was on the screen 29 ms after input had a move, on average, 33 ms at most, with commits made as the mouse moved.
- **`hc_events` writes ERDP every time**, events or not. drv.library's loop calls xhci's poll before the line's handler, so the poll can read the events and the handler find none after clearing IP: written only after events, EHB could stay set until the next timer. Seen in the code, not on the board, whose bursts were the moderation's.
- **A pointer moved alone cost "drag me" a frame**: committed at once, it held the window's next frame to the latch after (23% of its vblanks missed, with moves at 100 a second). intuition now commits a pointer moved alone 4 ms before the vblank (`POINTER_LEAD`), so a frame presented meanwhile is composed into the same commit. Not yet measured with a hand on the mouse. Committing the pointer even while a flip waits made it worse, as display programs the controller with the first commit after a latch: the composed frame waited all the same, and the pointer was 43 ms late.
- **The pointer is 16×16**, small on 1920×1080, and at the right or bottom edge it shows a pixel of itself, as its hot spot is its top left: it can seem lost. Not changed in V7.

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
