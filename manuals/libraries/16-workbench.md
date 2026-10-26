# 16. The Workbench and Prefs

*Written, V8 step 7.*

The desktop's two programs ([docs §13.5](../../docs/13-workbench.md#135-core-desktop-applications)). The **Workbench** (`servers/workbench`) is the backdrop under every window, with the volumes `dos` has mounted as icons, and a window of each volume or drawer opened. **Prefs** (`servers/prefs`) sets input's and intuition's settings, live, and saves them for the servers to read when they start. Both are ordinary programs: intuition's clients, drawing with gfx.library (chapter 12), their windows' gadgets ui.library's (chapter 15). This chapter is how they're built, as an example of a desktop program, and what a program that wants to work with them needs.

## Concepts

- **Started by hwdb.** On the desktop hwdb starts the Workbench with intuition, and keeps it as it keeps intuition: started again when it ends, until it Quits (ends with 0). Its Tools menu asks hwdb for a Shell (`HWDB_NEW_SHELL`, a console window, chapter 13) or Prefs (`HWDB_NEW_PREFS`, one at a time), which hwdb holds, so they outlive the Workbench. A program the Workbench starts itself, from a drawer or Execute Command, is its child, and ends with it.
- **The backdrop window.** A borderless `WFLG_BACKDROP` window under the screen bar, the screen's width: intuition keeps it behind every other. The Workbench draws the backdrop once into a bitmap of its own (§13.2's gradient, in intuition's backdrop colours, and three curved planes a step lighter, as paths), and copies from there whenever part of the window is drawn again.
- **Icons** (§13.4, v8-plan decision 3: drawn in code). Each volume `dos` lists (`NextVolume`), in its order, down the left, 24 px in and 88 px apart: **System** for the first that holds `EFI/planar` (the volume `SYS:` is), **Storage** for the rest. Each is drawn in paths on a 64 px canvas, labelled below in System Sans. One click selects an icon: its lid lifts 3 px, its rim or light brightens, and its label sits on a cobalt pill. Two within `WB_DOUBLE_NS` (500 ms) open it.
- **A drawer's window.** A volume or directory opened is a window of its own, sized by its corner, with a ui.library list of its entries (a drawer's with a `/` after it) and a line saying how many there are. A double click on a drawer opens its window; on a file, starts it as a program with no arguments, its directory its current one. A file that isn't a program says so in a requester.
- **The menus**, on every window of the Workbench's: **Workbench** (About; Execute Command..., GUI-E; Quit, GUI-Q), **Window** (Open Parent; Close, GUI-K) and **Tools** (Shell; Prefs). Execute Command opens a small window with a text field: its line runs as the Shell runs one, the first word a command in `C:` (or a path with a volume), the rest its arguments, `SYS:` its directory.
- **The HUD** (`ip_Hud`): a `UI_HudBadge` at the top right, the CPU busy and memory used, from `QuerySystem` once a second. It's a UI of its own whose root is a `UIPANE_BACKFILL` column, drawn on the backdrop by its backfill (chapter 15).
- **Prefs' window.** A split pane: input's settings on the dark side, a slider each (the repeat's delay and rate, the pointer's speed and acceleration), then display's mode and power, said not set (v8-plan decision 5: display's pane is to choose among the monitor's EDID modes once display can set one, power's a profile once it has performance control). intuition's on the light side: the HUD, the pointer's size, the backdrop's colours (Twilight, §13.2's; Ocean; Graphite). Then **Cancel** (put back what was in force when Prefs opened), **Use** (keep it, save nothing) and **Save** (write the files, then end). The close gadget is Cancel.
- **Live.** Each change goes to its server as it's made: `INPUT_SET` to input ([Devices ch. 7](../devices/07-input.md)), `SetPrefs` to intuition. Each server holds every field to its range and refuses a set out of one.
- **Saved.** `SYS:Prefs/Env-Archive/input.prefs` and `intuition.prefs`, small tagged files ([DOS ch. 1](../dos/01-files-and-programs.md#saved-settings)). Each server reads its own at its start, so a saved setting is back after the server dies and hwdb starts it again.

## The event loop

The Workbench has one connection to intuition and several windows on it; each event says its window. It waits with a deadline, for the HUD's second:

```c
for (;;) {
    uint64_t until = wb.hud_on ? wb.hud_next : CurrentTime() + SECOND;

    e = wb.intuition->WaitIntuiEvent(&wb.in, until);
    if (e == ERR_PORT_DIED || PortGone(&wb.in.in_Port))
        return 3;                           /* hwdb starts it again, on the next intuition */
    if (!events())                          /* every window's, to its own */
        break;                              /* Quit */
    hud_tick();
    children();                             /* programs it started that have ended, let go of */
}
```

Every window has one buffer, so before an event that may draw, the last frame must be safe (`WaitSafe`). Settings come as `IDCMP_NEWPREFS`: the Workbench reads them (`GetPrefs`) and draws the backdrop again if its colours or the HUD changed.

## Prefs, from a program

Prefs is how a user sets these, but nothing is Prefs' alone: any program may get and set the same settings. From `conformance/window.c`, which checks Prefs' clicks reached input:

```c
struct InputPrefs p = {};

input_prefs(INPUT_GET, &p);             /* on a connection to "input" */
if (p.ip_RepeatRate == rate && p.ip_PointerSpeed == speed)
    ...
IIntuition->GetPrefs(&in, &np);         /* intuition's */
```

## How they're tested

`make wb-test` draws, on the host, the backdrop with QEMU's two volumes' icons, one selected, a volume's window, the HUD on the backdrop (drawn again in part as whole), and Prefs' window, scalar and NEON, against CRCs from pictures looked at once (`build/workbench`). On QEMU, `conformance/window.c` step 6d starts the Workbench and reads the screen back as `wb.h` draws dos's volumes; opens the first volume and a drawer in it by double clicks, each window read back as `wb.h` draws what dos lists there; opens Prefs from the Tools menu; sets the repeat rate and the pointer's speed by clicks on their sliders, input agreeing and a move scaled by the speed; turns the HUD on; and saves. Its second round kills intuition and input, and each has the saved settings again from its file. hwdb says `V8.7 workbench ok` and `V8.7 prefs ok`.

## Rules

- **A program the Workbench starts ends with it.** Something that should outlive it is hwdb's to start, as the Shell and Prefs are.
- **Settings are set whole.** `INPUT_SET` and `SetPrefs` take every field: get them first.
- **Saving is Prefs'.** A server never writes its own file.
- **Names on a disk are ASCII**, long or short ([DOS ch. 1](../dos/01-files-and-programs.md)).

## Differences from the RKM

- **Icons are drawn in code**, not read from `.info` files (decision 3). A file in a drawer has a row, not an icon, until `.info` files come after V8.
- **One Prefs**, not an editor per subject (Input, Pointer, WBPattern, ScreenMode); and its files are each server's, read by the server, not IFF files read by `IPrefs` into intuition.
- **No `RAM:`**: dos mounts disks only, so the Workbench shows what it has.
- **Copy, rename, delete and the trash wait**, as v8-plan step 7 says.

## Functions used

[GetPrefs](../autodocs/intuition.md#intuitiongetprefs), [EasyRequest](../autodocs/intuition.md#intuitioneasyrequest), [OpenWindowTagList](../autodocs/intuition.md#intuitionopenwindowtaglist), [SetMenuStrip](../autodocs/intuition.md#intuitionsetmenustrip), [WaitIntuiEvent](../autodocs/intuition.md#intuitionwaitintuievent); [HandleUIEvent](../autodocs/ui.md#uihandleuievent), [AddGadget](../autodocs/ui.md#uiaddgadget); [NextVolume](../autodocs/dos.md#dosnextvolume), [ExamineNext](../autodocs/dos.md#dosexaminenext), [ReadPrefs](../autodocs/dos.md#dosreadprefs), [RunCommand](../autodocs/dos.md#dosruncommand); [QuerySystem](../autodocs/exec.md#execquerysystem).
