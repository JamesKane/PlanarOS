# Includes

*Planar Reference Manuals, volume 4.* The public headers, `include/planar/`. They are the system's C ABI ([docs §7.2](../../docs/07-language-toolchain.md)): any other language binds to Planar through them.

*Checked against V5; `gfx.h` and `font.h`, V8 step 3; `ui.h`, V8 step 6; `prefs.h`, V8 step 7.*

| Header | Contents | Used by |
|---|---|---|
| `planar/exec.h` | `Handle`; task, signal, Area, process and time calls; `AREAF_*` (with `AREAF_DMA32`), `AREAR_*`; `struct AreaInfo`; `PROCF_*`, `PROCS_*`, `PFAULT_*`; `struct ProcessInfo`; `struct ProcessImage` (`CreateProcessImage`); `struct SystemInfo` (`QuerySystem`); `SIGBREAKF_CTRL_C` | Every program |
| `planar/ipc.h` | Ports and connections: the kernel calls, `struct ConnArena`, `struct ConnRing`, `struct Message`, `struct MinNode`, `CONN_*`, `CONNF_*` | `lib/exec`; servers that manage rings themselves |
| `planar/lease.h` | Device leases: the lease, interrupt, DMA Area and firmware calls; MSIs (`AllocMsi`, `struct MsiInfo`); the console's input (`GrantConsole`, `ReadConsole`); the root's holder's `GrantRoot` and `ColdReboot`; `LEASE_IRQS`, `LEASES_*`; `struct LeaseInfo` (with `li_LentPages`), `struct FirmwareInfo` (with the framebuffer) | Drivers, `hwdb`, the Shell |
| `planar/drv.h` | hwdb's protocol with drivers: `HWDB_BIND`, `HWDB_STARTED`, `HWDB_STOP` (`DRV_STOP_MS`), anyone's `HWDB_UNRESPONSIVE`, intuition's `HWDB_KILL_CLIENT` and `HWDB_NEW_SHELL`, the Workbench's `HWDB_NEW_PREFS`, the desktop test's `HWDB_TEST_KILL` (test mode only), and the bus driver's `HWDB_PCI_HOST` and `HWDB_PCI_FUNCTION`; `struct HwdbMsg`, `struct DrvDevice` (registers, line, MSI-X, cells), `struct DrvRange`, `struct PciHost`, `struct PciFunction`, `struct PciWindow`, `struct PciBar`; `struct DrvRequest` (`DRV_READ`, `DRV_WRITE`); drv.library: `struct drv_ops`, `drv_main` and its helpers | Drivers, `hwdb`; console clients |
| `planar/devices/power.h` | The power class: `struct PowerMsg`, `struct PowerRef`, `POWER_ON`, `POWER_OFF`, `POWER_QUERY`, `POWER_DOMAIN` to `POWER_SUPPLY` ([Devices ch. 8](../devices/08-power.md)) | `hwdb`, `power`, drivers that vote |
| `planar/devices/display.h` | The display class: `struct DisplayMsg`, `struct DisplayPlane`, `struct DisplayPlaneInfo`, `struct DisplaySync`, `DISPLAY_*` ([Devices ch. 10](../devices/10-display.md)) | `display`, its clients |
| `planar/devices/host.h` | The host channel: `struct HostMsg`, `HOST_WRITE`, `HOST_READ`, `HOST_LINE` ([Devices ch. 5](../devices/05-drivers.md#virtio-mmio)) | `virtio-mmio`, the tests (`conformance/hostask.h`) |
| `planar/devices/input.h` | The input class: `struct InputMsg`, `struct InputRing`, `struct InputEvent`, `struct InputPrefs`, `INPUT_*` ([Devices ch. 7](../devices/07-input.md)) | `input`, `xhci`, the Shell, intuition, Prefs |
| `planar/devices/gpu.h` | The GPU class: `struct GpuMsg`, `struct GpuQueueIn`, `struct GpuQueueOut`, `struct GpuSync`, `GPU_*`, `GPU_CSF` and `GPU_ROGUE` ([Devices ch. 9](../devices/09-gpu.md)) | `gpu-panthor`, `gpu-rogue`, their clients |
| `planar/devices/block.h` | The block class's protocol: `struct BlkMsg`, `struct BlkReq`, `BLK_INFO`, `BLK_IO`, `BLK_READ`, `BLK_WRITE`, `BLK_FLUSH`, `BLK_EIO`, `BLK_BATCH`, `BLK_VERSION` | Block drivers, `lib/blk` |
| `planar/blk.h` | The block class's client library: `struct blk`, `blk_*`, `BLK_MSGS`, `BLK_GONE_MS` | Block clients: `dos`, `blkbench`, hwdb's kill test |
| `planar/dos.h` | The dos server's protocol (`struct DosMsg`, `struct DosEntry`, `DOS_*`, `DOSF_*`) and dos.library in the RKM's names: files (`struct Dos`, `OpenFile`, `ReadFile`), the loader (`RunCommand`), command lines and paths (`SplitArgs`, `ReadArgs`, `AddPart`), the console (`struct Con`, `PutStr`, `Printf`, `ReadCon`, `SetConName`) | Commands, the Shell, hwdb, conwin |
| `planar/prefs.h` | Saved settings: `struct PrefsHeader`, `PREFS_MAGIC`, `PREFS_VERSION`, `PREFS_TAGS`, the `PREFS_*` tags, `ReadPrefs` and `WritePrefs` ([DOS ch. 1](../dos/01-files-and-programs.md#saved-settings)) | Prefs; input and intuition, which read their own |
| `planar/acpid.h` | hwdb's protocol with `acpid`: `struct AcpidDevice`, `struct AcpidRes`, `ACPID_*` | `acpid`, hwdb |
| `planar/msg.h` | `struct MsgPort` (with `mp_Lost`), `MSGPORT_STASH` and Exec's message calls, with `ConnectPortUntil`, `DoMsgUntil` and `ReportUnresponsive` | Every program that sends or serves messages |
| `planar/syscalls.h` | Kernel call numbers (`SYS_*`), errors (`ERR_NOSYS` to `ERR_TIMEOUT`) and `DEADLINE_NONE` | Stubs; any program that tests an error |
| `planar/endian.h` | `get_be32`, `get_le64` and friends, for firmware formats | Drivers, `hwdb` |
| `planar/utility.h` | utility.library: `struct TagItem`, `TAG_*`, `struct Hook`, the case and comparison calls, `SNPrintf`, `struct Span`, `struct Str`, `STR()`, the checked readers; `struct UtilityIFace` ([Libraries ch. 5](../libraries/05-utility.md)) | Any program |
| `planar/gfx.h` | gfx.library: `struct BitMap`, `struct RastPort`, `struct TextFont`, `struct TextAttr`, `struct TmpRas`, `PIXFMT_*`, `BMF_*`, `BMA_*`, `BLEND_*`, `JAM1`/`JAM2`, `FPF_*`, `FSF_BOLD`; paths (`struct GfxPath`, `struct GfxPoint`, `PATH_*`, `PATHF_EVENODD`), masks (`struct GfxMask`) and gradients (`struct GfxGradient`, `GRADF_HORIZONTAL`); `struct GfxIFace` ([Libraries ch. 12](../libraries/12-gfx.md)) | Anything that draws; intuition and its clients |
| `planar/ui.h` | ui.library: `struct UI`, `struct UIGadget`, `struct UIChange`, `UI_*` (kinds), `UIA_*` (tags), `TOGGLE_*`, `ALIGN_*`, `UIALIGN_*`, `UIPANE_*`, `UICF_*`; `struct UIIFace` ([Libraries ch. 15](../libraries/15-ui.md)) | Programs with gadgets in a window: Prefs and the Workbench |
| `planar/intuition.h` | intuition's protocol and intuition.library: `struct Intuition`, `struct Window`, `struct IntuiEvent`, `struct IntuiSync`, `struct IntuiMsg`, `WA_*`, `WFLG_*`, `IDCMP_*`, `struct IntuiPrefs` (V8 step 7), `INTUI_*` (the chrome's and the screen bar's sizes); menus (`struct NewMenu`, `NM_*`, `CHECKIT`, `MENUNUM`, `ITEMNUM`) and requesters (`struct EasyStruct`); `struct IntuitionIFace` ([Libraries ch. 13](../libraries/13-intuition.md)) | Programs with windows; hwdb's window test |
| `planar/font.h` | font.library: `struct FontFace`, `struct FontMetrics`, `struct GlyphMetrics`, `struct ShapedGlyph`, `struct GlyphRun`, `struct LineFit`, `struct LineSpacing`, `FONT_*` (faces, features, warnings), `GLYPHF_*`, `LINEF_*`, `ERR_BADFONT`, `ERR_UNSUPPORTED`; `struct FontIFace` ([Libraries ch. 14](../libraries/14-font.md)) | Programs that shape text; gfx.library |

C's headers, for code written for other systems, are apart from these, in `include/clib/`: `assert.h`, `ctype.h`, `errno.h`, `inttypes.h`, `stdbit.h`, `stdio.h`, `stdlib.h` and `string.h`, which lib/clib implements ([Libraries ch. 11](../libraries/11-clib.md)). Only a program built with `-Iinclude/clib` sees them, and they're C11 as well as C23.

## What the headers promise

- **Layouts are fixed by `static_assert`.** `struct ConnArena` is one 64-byte cache line and `struct ConnRing`'s header two. A layout change bumps `CONN_VERSION`, and `AcceptPort` and `ConnectPort` refuse an arena of another version.
- **Call numbers and error values are meant to stay fixed** once a release has them, with new calls taking new numbers. **Proposed**: there has been no release yet, and nothing in `docs/` decides this.
- **`planar/devices/`** holds the classes' protocols, one header each: a class's layouts are its drivers' and clients' contract. `BLK_VERSION` and `DOS_VERSION` are checked in every message.
- **A call that makes a handle returns `int64_t`**: the handle, or a negative error. `FindTask` is the one exception still declared `Handle`.
- **The per-process limits aren't in a header.** 256 handles, 64 tasks and 32 connection ends are kernel constants (`kernel/process.h`), and so is a port's queue of 32 (`kernel/port.c`). A program learns of them from `ERR_LIMIT` and `ERR_FULL`.
- **Fixed-width types only.** No `long`, no `ULONG`/`APTR` (VQ18). Pointers are 64-bit on every Planar platform.
- **C23.** The headers use `constexpr`, `nullptr`, `bool` and `[[noreturn]]`, and need a C23 compiler.

## Planned

One page per header, RKM style: each structure with its fields' meanings, and each constant with its value. They will be written when the headers settle after V2; until then the headers' own comments are the reference.
