# Plan: a host channel for tests, and port watches

**Status: built (2026-10-26); decisions 1 to 6 taken as they leaned; steps 1 to 6 built on QEMU.** Two items from the simplify pass's altitude review (after V8 step 8's QEMU half), each a change to a mechanism that several programs work around today. Neither needs a board, so both can be done while the boards are busy. The decisions at the end are taken.

## Why

### 1. Tests ask the host for input over the rate-limited log

A test that needs the mouse, keys or a screendump prints a line, such as `window: send move 100 0`. `tools/boot-check.py` reads it from the console transcript (its `SEND` pattern) and acts through QEMU's QMP. The line goes through `KLog`, which `kernel/syscall.c`'s `sys_klog` limits to 32 lines at once and 20 a second after that, for each process. A line over the limit is counted and dropped, and `KLog` still returns 0 (the autodoc says so).

What that costs today:
- **Pacing by hand.** `conformance/window.c`'s `point_at` spaces its asks 50 ms apart, so that a 1280-pixel crossing isn't cut short by a dropped line (V8 step 7). `send drag`, `send keys`, `send text` and `send screendump` aren't paced, and window.c has 22 send lines and about 80 other log lines drawing on the same allowance. A test that logs more, for any reason, can lose a key press, and the failure shows up far from its cause.
- **No acknowledgement.** The test can't tell when the host has acted. It guesses:
  - it waits 300 ms after a screendump;
  - it polls intuition until the pointer moves, and takes 2 s without a move to mean nobody is at the mouse;
  - `boot-check.py` sleeps 30 ms after each key.
- **The transcript is the protocol.** Asks are scraped from the same stream as everything else. A test's ask can be split by another process's line, and a program's own output could be taken for an ask.

### 2. There's no way to be told that a port has appeared

`OpenPort` (and `ConnectPortUntil` on it) can wait for a name, and blocks while it does (V6 step 7, `kernel/port.c`'s `wait_name`, a waiter on the task's stack). A server that wants to know when another server comes up, while it goes on serving, has no call for that. So each one works around it:

| Program | Waiting for | How |
|---|---|---|
| `servers/input/main.c:264` | dos, to read its saved settings | a loader task blocked in `OpenDosUntil(DEADLINE_NONE)`, an atomic flag and a signal |
| `servers/intuition/main.c:1771` | dos, the same | another loader task, plus a synchronous copy of it at `:1790` for when dos is already up |
| `servers/intuition/main.c:293` | display and input, after either restarts | an awaiter task each, which **connects to the port to find it** and closes at once; the server accepts a connection for nothing |
| `drv/xhci/main.c:110` | input, after it restarts | tries `ConnectPort` every 500 ms (`RETRY`): the keyboard and mouse are dead for up to half a second after input's restart |
| `servers/conwin/main.c:185, 491` | intuition, after it restarts | tries every 50 ms, then every 100 ms |

So the program logic is spread across extra tasks, and some of them make throwaway connections. The xhci gap is visible: the kill test kills input, and `window.c` already has to retry its moves for up to 10 s afterwards (`survive`'s "until then a move is lost, and asked for again").

## Goal

- Tests ask the host for input on a channel of their own. Each ask is delivered, and is answered once it has been done. On a machine without the channel (a board), a test knows at once that there's nobody to ask.
- A program can ask to be signalled when a port of a given name exists, and keep serving meanwhile. Every loader task, awaiter task, probe connection and retry loop in the table above is replaced by a watch.

## Exit

- `conformance/window.c` and `conformance/inputtest.c` ask through the channel. `point_at` has no pacing, the 300 ms screendump guess is gone, and `boot-check.py`'s `SEND` scraping is removed. A mutation test proves the channel doesn't depend on the log: window.c logging 200 extra lines before each ask still passes.
- `KLog` returns `ERR_FULL` when it drops a line.
- `WatchPort` is a kernel call, with its autodoc and `conformance/portwatch.c`.
- `grep` finds no task that exists only to wait for a port, no `ConnectPort` that is closed at once to see whether a port exists, and no retry timer for a server's port in `servers/` or `drv/`.
- The kill test's `T_INPUT` measures input's restart to xhci's first report delivered again: under 20 ms on QEMU, against up to 500 ms now.
- `make test-all` is green, and the manuals are current: Devices ch. 1 (the driver), the autodocs (`exec.md`, `msg.md`) and Libraries ch. 4.

| Step | Content |
|---|---|
| **1. KLog says what it drops** | `sys_klog` returns `ERR_FULL` for a dropped line; the autodoc. |
| **2. The channel's device** | virtio-console (device ID 3) in `drv/virtio-mmio`, port 0 only, publishing `host.0`: a line written, a line read. Added to both QEMU machines (`mk/qemu-virt-*.mk`). |
| **3. The channel's ends** | `lib/hostchan`: `HostAsk(line, deadline)`, numbered asks, one at a time, reconnecting across the driver's restarts. `boot-check.py` serves the socket: it acts, then answers `done N`. |
| **4. Tests moved over** | window.c's and inputtest's asks go through `HostAsk`; the log pacing, the guessed waits and `SEND` go. The mutation test. |
| **5. WatchPort** | The kernel call and handle; `OpenPort`'s wait rebuilt on the same record; `conformance/portwatch.c`. |
| **6. Watches used** | input's and intuition's loader tasks, intuition's awaiters, xhci's retry and conwin's retry replaced; `T_INPUT`'s measure. |

Steps 1 to 4 (the channel) and 5 to 6 (watches) don't depend on each other. Step 6 needs 5; step 4 needs 2 and 3.

## 1. KLog says what it drops

`sys_klog` returns `ERR_FULL` when the line is dropped, and 0 otherwise. Nothing that calls `KLog` today checks its result, so nothing changes for them. `KPrintF` (conformance/log.h) passes the result on. The autodoc's RESULT gets corrected.

**Tested by:** `conformance/klograte.c` (new, a few lines): 33 lines at once, and the 33rd returns `ERR_FULL`.

## 2. The channel's device

- **virtio-console**, on the virtio-mmio transport `drv/virtio-mmio` already drives for the entropy device. Port 0 only, without `VIRTIO_CONSOLE_F_MULTIPORT`: a receive queue and a transmit queue, each a few page-sized buffers in the DMA Area as the rng's queue is.
- **The port it publishes is `host.0`**, not `con.N`. It's a line to whatever runs the machine, not a console: nothing else in the system should bind to it or log through it (decision 3).
- **The protocol** is `DRV_WRITE` (one line out) and `DRV_READ` (the next whole line in, waiting up to the request's deadline), as `drv.h`'s class requests already are.
- **QEMU:** `-device virtio-serial-device -chardev socket,id=host,path=$(BUILD)/host.sock,server=on,wait=off -device virtconsole,chardev=host`, in both QEMU machines' `QEMU_USB` group. That group already carries the QMP socket, so the hosted platform and the boards are left alone. On AArch64's EFI boot the device is in QEMU's DSDT, as the rng's is.
- **Supervised** as any driver: the kill test's rounds kill it with a client on it (step 3's library), and the client must recover.

**Tested by:** hwdb's kill test, with the channel as one more class client (`K_HOST` in `killtest.c`): it writes `ping N` and expects `pong N` from `boot-check.py`.

## 3. The channel's ends

- **`lib/hostchan`**, a static library for tests:
  - `HostOpen()`: `ERR_NOTFOUND` at once where there's no `host.0` (a board, the hosted build).
  - `HostAsk(const char *line, uint64_t deadline)`: writes `ask N <line>`, then reads until `done N ok` or `done N <error>`.
  - One ask at a time. On `ERR_PORT_DIED` it reconnects (`ConnectPortUntil`, a class library's recovery, §11.9) and asks again with the same N. The host answers a repeated N without acting twice.
- **`boot-check.py`** connects to `host.sock` (retrying until QEMU makes it).
  - **Asks:** it parses each `ask` line, does what it says through the existing `Qmp` methods, and answers `done N ok` once QMP has returned (for keys, after QEMU's 30 ms poll), or `done N` with the error. It echoes each ask to the transcript, prefixed `[host]`, so the log reads as before.
  - **Pings:** it answers `ping N` with `pong N`.
  - **The verbs** stay as they are (`keys`, `text`, `mouse`, `move`, `drag`, `screendump`).

## 4. Tests moved over

- **window.c:**
  - `point_at`, `click`, `dragged`, `typed` and the screendumps call `HostAsk`.
  - `point_at` loses its 50 ms pacing and its 2 s "nobody moved it" probe: `HostOpen`'s `ERR_NOTFOUND` is `NO_INPUT`.
  - The `nap(300)` after each screendump goes; the answer means the file is written.
- **inputtest.c** likewise.
- **`boot-check.py`'s `SEND` pattern and its scraping** go, after both are moved over.
- **The mutation:** window.c built with `-DLOG_FLOOD` logs 200 lines before each ask, and `make test` still passes. It's run once and recorded, not kept in `test-all`.
- **Measured:** the RV64 boot's time before and after. The guessed waits add up to several seconds today.

**Built (2026-10-26), steps 1 to 4, with what changed on the way:**
- **`KLog` returns `ERR_FULL`** for a line it drops, and `KPrintF` passes it on. The test is `conformance/logflood.c`, the V2.4 flood: of its 500 lines, at least 400 must say `ERR_FULL`. A separate `klograte.c` wasn't needed.
- **The device** is in `drv/virtio-mmio` as planned, publishing `host.0`. The driver now takes its devices' messages through drv.library's `message` callback, so a `HOST_READ` can be answered when a line comes. The entropy device's `DrvRequest`s go the same way, answered at once.
  - It has its own messages (`struct HostMsg` in `planar/devices/host.h`), not `DrvRequest`s, whose 64 bytes are too few for a line.
  - **A bug found on the way:** a write waits for its interrupt with `drv_wait_irq`, which takes the line's signal, and the line stays masked until it's acknowledged. The library's loop never saw that signal, so the host's answer never interrupted until the next write unmasked the line, and asks timed out at random. The write now gives the line back (`AckWaitIrq` with no mask). That's in drv.md now, for any driver that waits within a request.
- **The kill test's client** of `host.0` (`K_HOST`) writes an empty line, which the driver answers without the host, instead of the planned ping and pong. It's the supervision that's tested there: the client recovers from every death. The round trip through the host is tested by every ask the window test makes. On RV64 the console is a unit of its own, so the kill test now has 6 drivers and 6 clients.
- **The tests' end** is `conformance/hostask.h`, a header like `log.h`, not a `lib/hostchan`: only conformance programs ask.
  - `HostAsk(fmt, ...)` connects on its first ask without waiting, so a machine with no channel says `ERR_NOTFOUND` at once. Once connected, it waits to reconnect across the driver's restarts.
  - Asks are numbered from the clock's microseconds, so one program's numbers don't meet the last one's. The host answers a repeated number without acting twice.
- **`boot-check.py`** serves `--host`'s socket, says each ask on its own output as `[host] ...`, and no longer scrapes the transcript. On AArch64's EFI boot, edk2 writes its console escapes to the virtconsole before Planar starts, so an ask is found anywhere in a line.
- **window.c and inputtest.c** ask through it.
  - `point_at`'s pacing is gone, and three `nap(300)`s after screendumps.
  - The naps *before* screendumps stay: they wait for a frame to be shown, not for the host.
- **The mutation:** every ask preceded by 200 `KLog` lines (inputtest's show "189 lines dropped") still passes on RV64.
- **Measured:** RV64 and AArch64 (EFI) to `V8.8 done`.

## 5. WatchPort

```c
#include <planar/ipc.h>
int64_t WatchPort(const char *name, int64_t sigbit);    /* a handle, or an error */
```

- **What it does:** signals the calling task's `sigbit` once a port named `name` exists: at once if it already does, else when `CreatePort` makes it. It fires once. The caller then connects (`ConnectPort`, without waiting) and closes the handle with `CloseHandle`.
  - A port that's made and gone again before the connection gives `ERR_NOTFOUND`, and the caller watches again. That's the same race `port_open_call`'s loop takes care of now.
  - Closing the handle before it fires cancels the watch. A process's death closes its watches, as with any handle.
- **In the kernel:** a `struct name_waiter`, now `kmalloc`'d and owned by a handle (`HANDLE_WATCH`), on the existing `name_waiters` list. `CreatePort` already walks that list and wakes each match. It now posts the watcher's own signal instead of `SIGF_PORT`.
  - `OpenPort`'s wait becomes a watch on its stack with `SIGF_PORT`: one mechanism, not two.
  - **Limit:** at most `PROCESS_WATCHES` (16) per process, as `PROCESS_CONNS` limits connections. Beyond it, `ERR_LIMIT`.
- **Not watched:** a port's going. A connection already says that (`PortGone`, its break signal), and a program without a connection to it has nothing to lose (decision 4).

**Tested by** `conformance/portwatch.c`, which checks:
- a watch on a name nobody has, fired by another process's `CreatePort`;
- a watch on an existing port, fired at once;
- a watch closed before it fires, which never fires;
- three tasks watching one name, all fired;
- the 17th watch refused with `ERR_LIMIT`;
- a watcher killed with watches out: its handles and the kernel's records back where they were (`QueryProcess`, and the kernel's count, as V7's kill-window checks pages).

It's also mutation-tested: a `CreatePort` that forgets to post the watcher's signal fails the first check.

## 6. Watches used

Each of these is a signal in the program's main wait, followed by a connect, instead of a task or a timer:

- **input and intuition's saved settings:**
  - A watch on `dos` from the start. When it fires: `OpenDos`, `ReadPrefs`, `CloseDos`, and the settings taken unless a client has set some (as now).
  - The loader tasks, `saved.done`, `SIG_PREFS` and intuition's synchronous second copy go.
  - **Caveat:** the read happens in the server's own task. It's one small file, under 10 ms on QEMU. If that matters on a board, decision 5 is the fallback.
- **intuition's display and input:** `await()` and the awaiter tasks become a watch on each name. That also ends the probe connections that display and input accept for nothing.
- **xhci:** after input dies, a watch on `input`; when it fires, connect and attach the devices. `RETRY` goes.
- **conwin:** after intuition dies, a watch on `intuition`; when it fires, `open_window`. The 50/100 ms retry goes.
- **window.c's `survive`:** its 10 s of retried moves after input's death become a single wait for intuition's `im_InputOpens`. Once the channel answers each ask, a lost move fails at once instead of being guessed at.

**Measured by** `T_INPUT`: the time from input's restart to the first report xhci delivers on the new connection. It's under 20 ms on QEMU; today it's up to 500 ms. The boards' figure is recorded when they're next on the bench (the V8 step 8 runs).

**Built (2026-10-26), steps 5 and 6, with what changed on the way:**
- **`WatchPort`** is kernel call 60: `kernel/port.c`'s `struct name_waiter`, now with its own signal, a handle (`HANDLE_WATCH`), its process and a copy of the name. `CreatePort` posts each waiter's own signal. It takes `OpenPort`'s waiters off the list, and leaves a watch on it, fired, until it's closed. `CloseHandle` closes a watch (`port_watch_close`), and `ipc_task_gone` frees a leaving task's watches. Each process may hold 16 (`PROCESS_WATCHES`).
  - **`conformance/portwatch.c`** runs in V6.7 beside `deadline.c`, and the selftest checks the system's handle count is back after both.
  - **The mutation:** a `CreatePort` that doesn't post to watches fails `portwatch` at its first wait.
- **drv.library's loop** wakes for a driver's own signals too (`drv_ops.signals`, new). xhci's are its connection to input, signalled when input goes, and its watch on input's port, for its coming back. The 500 ms `RETRY` is gone.
  - **An old bug found on the way:** xhci's `poll` tried to connect before it looked whether input had gone, so the death was handled a poll late. With the old retry that hid in its 500 ms. On AArch64, where xhci's loop sleeps between interrupts, it cost about 200 ms until the order was swapped.
  - **Measured by `T_INPUT`**, input's restart to its devices attached again (to the 5 ms of hwdb's loop): 23 ms on RV64 and 15 ms on AArch64 (EFI), against 495 ms with the retry; across `make test-all`'s eight boots with xhci, 13 to 30 ms, and 58 on one. That's not quite the exit's 20 ms everywhere: the figure includes hwdb's own polling.
- **intuition's awaiter tasks** are watches on `display.0`, `input` and `dos`, made again by `await()` each time one's needed. The probe connections are gone, and so is the loader task: the saved settings are read in the main task when dos's watch fires (decision 5).
- **input's loader task** is likewise a watch on `dos`. `SIG_PREFS` is the watch's signal, read from `WaitPorts`' result.
- **conwin's 50/100 ms retry** is a watch on `intuition`, made again after each failed try. A window refused with intuition there is still tried again in 100 ms: that's not a port that's missing.
- **Left as they are:**
  - **The GPU drivers' 500 ms `RETRY`** (`drv/gpu-panthor`, `drv/gpu-rogue`) waits for the firmware file on dos, not only dos's port, and runs only on the boards.
  - **The Shell's half-second retry for input** is in `cmd/`, outside this plan's `servers/` and `drv/`.
  - **`window.c`'s retried moves after input's death** in `survive` stay as a deadline loop, now over moves that are answered.

## Size

- **The channel:** about 450 lines.
  - the driver's virtio-console: 250;
  - `lib/hostchan`: 120;
  - `boot-check.py`: 80;
  - minus about 60 from window.c and inputtest.
- **Watches:** about 400 lines added and 250 removed.
  - the kernel: 150;
  - `portwatch.c`: 250;
  - removed from the users: tasks, flags and timers.

It's smaller than a V8 step. Each half could be done on its own, in either order.

## Sources

- virtio 1.2 §5.3, the console device; §4.2, MMIO.
- QEMU's `virtio-serial-device` and `virtconsole` documentation, and its QMP reference for `input-send-event` and `screendump`, as `boot-check.py` uses them now.
- docs §11.9 (a class library's recovery), V6 step 7 (OpenPort's wait), V7 step 6 (the kill test's input phase).

## Decisions

All six taken as they leaned (2026-10-26).

1. **The channel's transport.**
   - **(a)** virtio-console on virtio-mmio, a socket to `boot-check.py`.
   - **(b)** The UART's receive side: `boot-check.py` types answers into the serial line. But the serial Shell (`C:shell SERIAL`) and `con.0` read that line, so answers and typed commands would mix.
   - **(c)** A second UART with QMP's `ringbuf-read`/`ringbuf-write`, which keeps it all on QMP. But neither QEMU machine has a second UART, and adding one means a device tree and DSDT change on each.

   *Leaning:* (a). It's a device and driver Planar would want anyway, for a host's file or clipboard later, and it touches no console.
2. **Should a dropped `KLog` line block instead?** Waiting for a token would make the log lossless, but then a server that logs in a loop is stalled by its own logging, and §12's servers mustn't be slowed by their diagnostics. *Leaning:* no. It keeps dropping and says so (`ERR_FULL`). Anything that must arrive goes on the channel.
3. **The channel's port name and class.** `con.N`, so a Shell could run on it, or `host.0`, its own class. *Leaning:* `host.0`. A Shell on it is easy to add later (`C:shell` on a named console), and until then nothing binds to it by accident.
4. **What `WatchPort` watches.**
   - **(a)** A port appearing only, fires once, a handle [as above].
   - **(b)** Also a port going.
   - **(c)** A flag on `ConnectPort` that returns at once and signals once it has connected, folding the watch into the connection.

   *Leaning:* (a). A port's going is a connection's business already. (c) puts a half-made connection in the client's hands, with its arena mapped and unanswered: more state for the same job.
5. **Where saved settings are read.** In the server's task when the watch fires, or still in a task, now woken by the watch. *Leaning:* the server's task. It's one file read of a few hundred bytes, once per start. If a board measures more than a frame, it moves back into a task, woken by the watch.
6. **When.** Now, before V8 step 8's board runs, or after V8's exit. *Leaning:* now, while the boards are out of reach. The board runs then measure xhci's recovery with the watch in, and the CIX's long kill-test runs are spared input's 500 ms gaps.
