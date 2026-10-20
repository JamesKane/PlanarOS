# 13. Workbench and desktop environment

**Status: proposed (2026-10-20).** The kernel IPC and memory primitives are built (V1, V2). The `display` and `input` servers are scheduled for V4 ([§8](08-roadmap.md)), and GPU queue leases for V5 ([§5](05-coprocessors.md)). This document specifies the architecture, visual design language, widget toolkit and iconography of the Planar Workbench.

Planar's desktop environment—**Workbench**—gives a single user direct, tactile control over the machine. Its visual language updates the classic screen-and-window metaphor with high-DPI brushed metallic surfaces, deep slate wells, bevelled steel-blue window frames and crisp vector iconography, while keeping all Amiga trademarked marks out of the system. Underneath, it uses Planar's single address space (P2), zero-copy message rings (P4) and process-level crash isolation ([§12](12-let-it-crash.md)).

---

## 13.1 Architecture and data path

### Layer split

Following P3 (*the kernel provides mechanism and servers provide policy*), neither the kernel nor the `display` driver knows what a window, a button or a menu is.

```
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ Core apps      Workbench (desktop/filer) · Shell · Prefs · SysMon       │
 ├─────────────────────────────────────────────────────────────────────────┤
 │ Libraries      ui.library (gadgets, layout, menus, themes)              │
 │ (caller RX)    gfx.library (2D paths, text, blits, damage regions)      │
 ├─────────────────────────────────────────────────────────────────────────┤
 │ Servers        intuition (screens, windows, hit-testing, composition)   │
 │ (own domain)   display (scanout, DPU planes, vblank flips, cursor)      │
 │                input (USB HID via xHCI/DWC3 → input event rings)        │
 └─────────────────────────────────────────────────────────────────────────┘
```

| Component | Kind | Role |
|---|---|---|
| `display` | Server | Owns the display controller (K1 DPU, Linlon/komeda, VOP2, MDSS). Exposes hardware scanout planes, the hardware cursor plane, and vblank flip notifications ([§2.4](02-architecture.md#24-servers-initial-set), [§5.3](05-coprocessors.md#53-by-board)). |
| `input` | Server | Reads USB HID keyboard, mouse and tablet reports and streams normalised `InputEvent` structs into a connection ring held by `intuition`. |
| `intuition` | Server | Owns the screen stack, window z-order, window chrome, global screen menu bar, focus and pointer hit-testing. Composes client surfaces (or assigns hardware planes) and posts flips to `display`. |
| `gfx.library` | Shared library | Runs in the caller's protection domain. Renders vector paths, bevels, gradients, damage rects and shaped glyph runs into the window's shared backing `Area`, using CPU SIMD (RVV 1.0 / NEON, VQ22) or the app's GPU queue lease ([§5.1](05-coprocessors.md#51-the-abstraction)). |
| `ui.library` | Shared library | GadTools-style retained widget toolkit running in the caller's domain: toggles, proportional sliders, segmented controls, scrollers and split panes. |
| `workbench` | Application | The desktop backdrop, volume and drawer manager, launcher and optional system telemetry badge. |

### Zero-copy window surfaces

Every window is backed by one or more page-aligned `Area`s allocated by the client and shared read-only with `intuition` (`ShareArea` with `AREAF_READ`).

```
Client process                         intuition server                display server
──────────────                         ────────────────                ──────────────
1. AllocArea(w × h × 4)
2. Render via gfx.library / GPU
3. ShareArea(area, READ) ──Present──▶  4. Validate damage rect
                                       5. Composite chrome + client
                                          (or map directly to DPU plane)
                                       6. Flip at vblank ────────────▶ 7. Program DPU
                                                                          scanout addr
```

1. **No pixel copies across IPC.** The client draws into its `Area` and sends a small `PresentWindow` message on its `intuition` connection ring containing the `Area` handle, a damage rectangle list, and a 64-bit frame sequence number.
2. **Double buffering by handle swap.** While `intuition` reads buffer *A*, the client draws the next frame into buffer *B*. When `intuition` finishes composition (or scanout leaves buffer *A* at vblank), it replies on the connection ring, returning *A* to the client.
3. **Direct hardware plane promotion.** Each target DPU has multiple hardware overlay planes (e.g. Linlon on CIX P1, VOP2 on RK3588, K1 DPU on RV2). When a client window is opaque, unoccluded, and matches a supported scanout format, `intuition` can hand its physical `Area` address straight to `display` as a hardware overlay plane, bypassing composition completely (**VQ31**).
4. **Hardware cursor plane.** The pointer sprite lives in a dedicated small `Area` bound to the DPU's cursor plane. `intuition` updates cursor $(x, y)$ coordinates on mouse interrupts without dirtying or re-compositing any window surface.

### Crash containment (`let it crash` for the GUI)

Under [§12](12-let-it-crash.md), a crashing GUI application must never freeze the pointer, corrupt the screen, or tear down `intuition`:

- **Client crash.** When an application crashes, the kernel sets `CONNF_CLIENT_GONE` on its `intuition` connection (`kernel/port.c`). `intuition` wakes immediately, unlinks all windows owned by that connection, unshares their backing `Area`s, and marks the occluded screen rectangle dirty for the next vblank. The dead window vanishes cleanly on the very next frame.
- **No shared-memory locks (F3).** `ui.library` and `gfx.library` run entirely in the client's domain. Client and server never share a mutex or widget tree in memory; all window management and input delivery travel over the connection ring.
- **Window chrome ownership.** `intuition` draws the outer window frame, title bar, depth/close gadgets and proportional border scrollers itself, so a hung client task can still be dragged, depth-arranged, or closed by the user. Clicking the close gadget sends `IDCMP_CLOSEWINDOW`; if the client does not close or reply within a configurable timeout, `intuition` offers to `KillProcess` the domain.

---

## 13.2 Visual design system

The visual language combines tactile bevelled geometry with deep twilight slate and brushed aluminium surfaces. Every surface has a clear light source from the **top-left** (`1px` specular inner highlight on top and left edges, `1px` dark shadow on bottom and right edges).

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ◉ PlanarOS   Workbench ▾   Window   Icons ▾   Tools ▾          ◔ 12:38 PM  ⚙ │ ◀── Screen bar (28 px)
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                ┌───────────┐ │
│  ┌──────┐                                                      │ CPU  0.8% │ │ ◀── Telemetry badge
│  │ 芯片 │     ┌──┬───────────────────────────────────┬──┬──┬──┐│ RAM 0.00% │ │
│  └──────┘     │▤ │ Prefs                             │▂ │◰ │✕ │└───────────┘ │
│  Ram Disk     ├──┴──────────────────┬────────────────┴──┴──┴─┬┤              │
│               │ Controls     (━━━●) │ Modes                  ││              │
│  ┌──────┐     │─────────────────────│ ┌──────────┬──────────┐││              │
│  │  ⚙   │     │ Sliders   ━━━█━━━━━ │ │  Active  │  Normal  │││              │
│  └──────┘     │                     │ └──────────┴──────────┘││              │
│   System      │ Toggles      (●━━━) │                        ││              │
│               │                     │               ┌──┬─────┴┴──────────────┤
│  ┌──────┐     │                     │               │▤ │ Shell               │
│  │ ▬▬▬  │     ├─────────────────────┴───────────────┤──┴─────────────────────┤
│  └──────┘     │ ◄██████████████████░░░░░░░░░░░░░░░► │ /System/Shell          │
│  Storage      └─────────────────────────────────────┤ planar:~> █            │
│                                                     │                        │
│  ┌──────┐                                           │                        │
│  │ ┼──┼ │                                           │                        │
│  └──────┘                                           └────────────────────────┘
│   Prefs                                                                      │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Colour palette

All colours are specified in sRGB (`ARGB8888` in scanout memory).

| Token | Hex (top → bottom / state) | Usage |
|---|---|---|
| `bg.backdrop.base` | `#182038` → `#272248` | Desktop vector backdrop base gradient (deep indigo-slate to twilight violet). |
| `bg.backdrop.plane` | `#222D4A` / `#312958` | Overlapping curved geometric planes on the desktop backdrop. |
| `bar.screen.bg` | `#DCE3EF` → `#9AA8C0` | Global screen title bar vertical metallic gradient. |
| `bar.screen.border` | `#FFFFFF` (top), `#141A26` (bottom) | `1px` specular top edge and `1px` dark bottom divider on the screen bar. |
| `bar.screen.text` | `#141824` | Screen bar title, menu labels, clock and status icons. |
| `win.frame.active` | `#5B88C2` → `#3A6298` | Active window title bar and border scroller tracks (Workbench steel blue). |
| `win.frame.inactive` | `#4A5668` → `#333C4B` | Inactive window title bar and border scroller tracks. |
| `win.bevel.light` | `#8BB4E8` (active), `#6A788E` (inactive) | `1px` inner highlight along top/left of title bar, borders and window gadgets. |
| `win.bevel.dark` | `#162030` | `1px` outer border and inner shadow along bottom/right of window frames. |
| `pane.dark.bg` | `#36435A` → `#46556E` | Primary dark-slate control well (left pane of split windows). |
| `pane.light.bg` | `#D8DEE8` → `#B8C2D1` | Secondary brushed-silver inspection pane (right pane of split windows). |
| `pane.terminal.bg` | `#121822` (92% alpha or solid) | Shell and code surface background with subtle ambient sheen. |
| `ctrl.accent.amber` | `#F08C20` → `#D97010` | Active primary toggle track, selected segment pill, primary focus highlight. |
| `ctrl.accent.cobalt` | `#3B82F6` → `#2563EB` | Active secondary toggle track, slider fill bar, progress indicator. |
| `ctrl.neutral.track` | `#253144` / `#526078` | Recessed slider trough and disabled/neutral toggle track. |
| `ctrl.thumb.light` | `#F4F7FB` → `#CFD8E6` | Raised toggle thumb, slider knob and unselected segmented button surface. |
| `text.light` | `#F0F4FA` | Primary label text on dark slate panes and active window title bars. |
| `text.muted` | `#7E8CA3` | Disabled control labels and secondary telemetry captions. |
| `text.dark` | `#1A2230` | Primary label text on light brushed-silver panes and unselected pills. |
| `text.shell.prompt` | `#4ADE80` | Shell prompt (`planar:~>`) phosphor green. |
| `cursor.fill` | `#E02828` → `#A81818` | Pointer arrow body with `#FFFFFF` `1px` outline. |

### Typography

- **UI sans-serif (`System Sans`)**: Clean humanist/geometric sans-serif at `13 px` (`16 px` line height) for menus, window titles, icon captions and gadget labels; **Bold** for the `PlanarOS` screen-bar badge and active window titles; **Regular** for menu items and control labels.
- **Terminal monospace (`System Mono`)**: Crisp programmer monospace at `13 px` (`18 px` line height) for `Shell` and kernel log viewers, with box-drawing coverage and zero ambiguity between `0`/`O` and `1`/`l`/`I`.
- **Label treatments**:
  - On dark slate panes (`pane.dark.bg`) and desktop icons: `#F0F4FA` text with a `1px` `(0, +1)` `#0E131D` drop shadow.
  - On light silver panes (`pane.light.bg`) and the screen bar: `#141824` text with a `1px` `(0, +1)` `#FFFFFF` (40% alpha) etched highlight.

---

## 13.3 Screen bar, windows and gadgets

### Global screen bar (`ScreenBar`)

Following the classic Intuition screen model, the top `28 px` of the screen is a single global bar owned by `intuition`:

1. **System mark and screen title (far left)**: The faceted blue Planar torus mark (`18×18 px`) followed by **PlanarOS** in bold (`#141824`). Clicking the mark opens the system menu (About Planar, Screen Mode, Reboot, Shutdown).
2. **Active window menu strip**: Immediately to the right of `PlanarOS`, `intuition` renders the menu strip registered by whichever window currently holds input focus. When the Workbench backdrop or a standard system window is active, the strip shows:
   - `Workbench ▾` (Backdrop, Execute Command…, Redraw All, Update, About, Quit)
   - `Window` (New Drawer, Open Parent, Close, Clean Up, Snapshot, Select Contents)
   - `Icons ▾` (Open, Copy, Rename…, Information…, Format Volume…, Empty Trash)
   - `Tools ▾` (Shell, Prefs, System Monitor, Hardware Graph, Kernel Log)
3. **Status tray (far right)**: A monochrome vector clock glyph, the current local time (`12:38 PM`), and a precision gear glyph that opens `Prefs` directly.

### Window chrome and border scrollers

Every standard `intuition` window consists of a bevelled steel-blue frame surrounding the client `Area`:

```
┌──┬──────────────────────────────────────────────────────┬──┬──┬──┐
│▤ │ Window Title                                         │▂ │◰ │✕ │  26 px title bar
├──┼──────────────────────────────────────────────────────┴──┴──┼──┤
│  │                                                            │  │
│  │                                                            │█ │  16 px right border
│  │                   Client Surface Area                      │█ │  with vertical
│  │                                                            │░ │  proportional track
│  │                                                            ├──┤
│  │                                                            │▲ │  Up / down step
│  │                                                            │▼ │  arrows
├──┼───────────────────────────────────────────────────┬──┬──┬──┼──┤
│  │ ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │◄ │► │  │◢ │  16 px bottom border
└──┴───────────────────────────────────────────────────┴──┴──┴──┴──┘  + resize corner
```

- **Title bar (`26 px` tall)**:
  - **Left cell (`24×20 px`)**: Recessed bevelled badge showing the window's class glyph (drawer/document glyph for Workbench and Prefs, terminal prompt glyph for Shell).
  - **Title region**: Drag handle for moving the window; double-clicking shade-rolls or zooms the window.
  - **Right gadget cells (three `22×20 px` bevelled buttons)**:
    1. **Iconify (`_`)**: Collapses the window into a desktop or dock icon while keeping the process running.
    2. **Depth / Zoom (`◰`)**: Left-click toggles the window between the front and back of the screen's z-order (classic Workbench depth arrangement); Shift-click toggles between user size and full size.
    3. **Close (`✕`)**: Sends `IDCMP_CLOSEWINDOW` to the client port.
- **Border scrollers (`16 px` right and bottom borders)**:
  - Scrollable windows place their proportional scrollbars **inside the right and bottom window borders** rather than consuming interior client space.
  - The track is a recessed dark-blue well (`#25344A`) with a raised bevelled steel-blue thumb (`#5B88C2`).
  - Stacked step arrows (`▲` / `▼` for vertical, `◄` / `►` for horizontal) sit tightly against the bottom-right corner above and to the left of the resize gadget (`◢`).

### Widget toolkit (`ui.library`)

`ui.library` provides six core gadgets matched to the mood board:

1. **Split-pane container (`UI_SplitPane`)**: Divides a window into a dark slate category/control well (`pane.dark.bg`, left) and a light brushed-silver detail pane (`pane.light.bg`, right), separated by a `2px` vertical bevel groove (`#1E2736` left, `#E8EEF7` right).
2. **Etched section dividers (`UI_Divider`)**: `2px` horizontal groove (`#242E40` over `#50607A`) separating logical control groups inside a dark pane.
3. **Pill toggles (`UI_Toggle`)**: `52×22 px` capsule with a `3px` recessed inner shadow and a `24×18 px` raised thumb (`ctrl.thumb.light`). Supports three semantic variants:
   - *Cobalt switch*: Blue track (`#2E7CF6`) when on, dark slate (`#253144`) when off.
   - *Amber switch with inline state*: Signal amber track (`#EB861E`) on the active side, neutral slate (`#58657A`) on the opposite side, optional inline glyphs (`|||`, `%`, `L/R`) or numeric step badges (`1`, `2`).
   - *Disabled switch*: Low-contrast slate (`#5A6578`) with muted grip marks (`|||`) and `#7E8CA3` label.
4. **Proportional sliders (`UI_Slider`)**: `6 px` recessed horizontal track (`#232F42` with `1px` bottom highlight `#566884`), filled left of the thumb with `#3B82F6`, and a `14×18 px` raised white-silver rounded thumb (`#F0F4FA` with `#1A2434` border).
5. **Segmented mode selector (`UI_Segmented`)**: `26 px` tall capsule container (`#DCE3EF` with `1px` dark outer border `#6E7B90`) holding two or more mutually exclusive options. The active option fills with Signal Amber (`#EB861E` → `#D46B0E`) and `#141824` medium-weight text.
6. **Telemetry HUD overlay (`UI_HudBadge`)**: Frameless semi-transparent dark-slate card (`#232C40` at 80% alpha, `6 px` corner radius, `1px` `#566480` border) pinned to the top-right of the backdrop, displaying right-aligned monospace readouts (`CPU 0.8%`, `RAM 0.00%`) refreshed at `1 Hz` via `QuerySystem` ([§2.2](02-architecture.md#22-kernel-responsibilities)).

---

## 13.4 Iconography (no Amiga trademarks)

> **Policy (decided):** Planar uses **none** of the Amiga trademarked or trade-dress icons. In particular, the stylised italic `'A'`, the red-and-white chequered Boing ball, and the rainbow checkmark/tick mark (which appeared in the raw mood-board generation on `Ram Disk` and `Prefs`) are **prohibited** in code, assets and documentation.

All system icons use Planar's **isometric tactile hardware aesthetic**: a `64×64 px` nominal canvas (`128×128 px` at 2× DPI), rendered at a `30°` isometric projection with a dark drop shadow, bevelled metallic edges and a distinctive accent glow per category.

| Icon | Palette | Visual specification |
|---|---|---|
| **Planar mark** (Screen bar) | Cobalt & ice blue (`#2563EB` → `#93C5FD`) | Faceted geometric hexagonal torus / aperture ring (`18×18 px` in the top-left of the screen bar). Represents a flat plane and shared rings. |
| **Ram Disk** (`RAM:`) | Dark slate carrier (`#2B3448`) + ice-blue silicon die (`#60A5FA`) | Isometric memory module / silicon die package with a glowing cyan circuit grid on the top surface and gold contact pins along the lower bevel. *(Replaces the mood board's 'A' glyph.)* |
| **System** (`SYS:`) | Electric cyan & cobalt (`#38BDF8` → `#1D4ED8`) | Isometric two-tier anodised blue system slab topped with a machined six-tooth precision gear and a glowing cyan rim between the upper and lower plates. |
| **Storage** (`DH0:` / `NVME0:`) | Signal amber & crimson (`#F97316` → `#B91C1C`) | Isometric warm-amber and deep-red solid-state storage stack with a recessed orange data plate on top and a glowing red activity light along the front bevel. |
| **Prefs** (`SYS:Prefs`) | Brushed silver (`#CBD5E1`) + slate base (`#334155`) + amber/cobalt accents | Tilted metallic control deck featuring two parallel recessed tracks—one with a Signal Amber thumb knob and one with a Cobalt Blue thumb knob. *(Replaces the mood board's rainbow checkmark.)* |
| **Shell** (`SYS:System/Shell`) | Obsidian slate (`#141A24`) + steel-blue frame (`#5B88C2`) | Isometric dark terminal slate displaying a phosphor-green `~> █` prompt glyph inside a bevelled steel-blue border. |
| **Drawer** (Folder) | Steel blue (`#4B74AA` → `#2E4C74`) + silver tab | Isometric bevelled metallic tray/drawer that slides open by `4 px` in its selected state. |

### Two-state icon interaction

Maintaining the classic Workbench feel, every desktop and drawer icon has two visual states:
- **Normal (unselected)**: Standard lighting and ambient drop shadow.
- **Selected / Active**: When clicked once, the icon's accent rim brightens (increased specular luminance on the cyan/amber/cobalt glow), the isometric lid or control element shifts subtly (`2–4 px`), and the label underneath draws a pill highlight (`#2563EB` at 60% alpha). Double-clicking opens the volume, drawer or tool.

---

## 13.5 Core desktop applications

The reference desktop comprises three first-party programs that ship in the base image once V4/V5 display and input are up:

### 1. `Workbench` (Desktop, volumes and drawers)

- Runs as a normal user-mode process supervised by `init` ([§12.7](12-let-it-crash.md)).
- Queries `dos` for mounted volumes (`RAM:`, `SYS:`, `EFI:`, `NVME0:`) and places their icons in a vertical column along the left edge (`24 px` left margin, `88 px` vertical pitch).
- Renders the procedural vector backdrop (`bg.backdrop.base` + `bg.backdrop.plane` Bezier curves) so the wallpaper costs zero disk space and scales cleanly to any resolution from 1080p to 4K.
- Hosts the optional top-right `CPU` / `RAM` telemetry badge (`1 Hz` update from `QuerySystem`, zero allocations in steady state).

### 2. `Prefs` (System preferences)

- Single-window or drawer-based control panel using `UI_SplitPane`:
  - **Left pane (dark slate well)**: Real-time interactive sliders and toggles for input speed, pointer acceleration, display refresh/vsync, audio volume and window shadows.
  - **Right pane (brushed silver well)**: Mode selectors (`UI_Segmented` with Signal Amber active pill) for memory/scheduler policy (e.g. big/little core placement bands from [§2.7](02-architecture.md#27-smp)), display plane promotion, and numbered step switches.
- Changes apply immediately via messages to `intuition`, `display`, `input` or `power`, and persist to `SYS:Prefs/Env-Archive/` when saved.

### 3. `Shell` (Command line and diagnostics)

- Connects to a pseudo-console port hosted by `intuition` (or runs directly on the framebuffer in V4 before `intuition` starts).
- Displays the current working assign/path header and a coloured prompt (`planar:~> `) in `#4ADE80` with a solid `#F0F4FA` block cursor.
- Supports both standard vertical/horizontal border scrollers for scrollback and zero-copy pipe/file redirection through `dos.library`.

---

## 13.6 Roadmap integration

| Milestone | Scope | Verification |
|---|---|---|
| **V4 (Framebuffer shell)** | `display` server (K1 DPU, Linlon) + `input` server (USB HID) + standalone `Shell` rendering with the `Shell` colour palette and monospace font ([§8](08-roadmap.md)). | Boots from storage to an interactive framebuffer shell on RV2 and CIX P1. |
| **V5.5 (Intuition + Workbench)** | `intuition` server, `gfx.library`, `ui.library`, hardware cursor plane, `ScreenBar`, `Workbench` backdrop/icons, `Prefs` and windowed `Shell`. | Window creation, dragging, depth-arrangement, zero-copy `PresentWindow` flips at 60 Hz, and clean window reclamation when a client process is killed (`kill-window` conformance test). |

---

## 13.7 Open questions

- **VQ31 Direct DPU plane promotion vs. full-screen composition.**
  All four target SoCs have multi-plane display controllers (K1 DPU, Linlon/komeda, VOP2, MDSS). Should `intuition` assign each visible top-level window to a hardware DPU plane when the window count fits the plane budget, falling back to CPU/GPU composition only when windows exceed planes or use translucency?
  *Leaning:* always use the dedicated hardware cursor plane for the red pointer; compose windows into one primary scanout buffer first in V5.5, then promote full-screen or video/3D queue-lease windows to dedicated overlay planes.

- **VQ32 Icon asset representation.**
  Should icons be stored in sidecar `.info` files (classic Workbench style, works on FAT32 as well as the native filesystem) using a compact binary vector bytecode so they scale cleanly to any DPI, or as pre-rasterised `64×64` and `128×128` `ARGB8888` images?
  *Leaning:* `.info` sidecar files (so FAT32 volumes carry icons without special filesystem attributes) containing a tiny binary vector command stream plus a cached `64×64` bitmap.

- **VQ33 Window translucency (`Shell` backdrop blur).**
  The mood board shows subtle backdrop bleed-through behind the `Shell` terminal body. On boards where `intuition` composites on the CPU (before GPU queue leases are active on every board), real-time Gaussian blur is expensive.
  *Leaning:* simple alpha blending (`92%` `#121822`) or a pre-blurred downsampled copy of the static procedural wallpaper when on CPU composition; optional real-time Kawase blur when `intuition` holds a GPU queue lease.
