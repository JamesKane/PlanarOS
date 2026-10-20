#!/usr/bin/env python3
"""Boot an image on a board over its serial console, through U-Boot.

    serial-boot.py --port /dev/cu.usbserial-X --image build/ky-x1/planar.bin
                   [--bootargs 'layout=global reboot'] [--log FILE]

Waits for a U-Boot prompt (sending Ctrl-C to stop distro boot), sends the
image with `loadx`, stops U-Boot's watchdogs, sets `bootargs`, runs
`booti` and echoes what the kernel prints to stdout, so tools/boot-check.py
can watch it as it watches QEMU. Exits 0 when the kernel reboots and
the board comes back (U-Boot's banner), 1 on a Guru Meditation, a failed
reboot, or if the run goes quiet for --quiet seconds. Everything the board sends is also written to the log.

The stock U-Boot on the Orange Pi RV2 autoboots with no delay and ignores
keys during it, so with no boot media it reaches the prompt after about
80 s (much longer with an Ethernet cable plugged in). Nothing else may have
the port open while this runs. From omega's tools/serial-boot.py.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xmodem  # noqa: E402

LOAD_ADDR = "0x200000"
PROMPT = b"=> "
PROMPT_WAIT = 600       # seconds to wait for U-Boot's prompt


class Board:
    def __init__(self, dev: str, log_path: str):
        self.fd = xmodem.open_port(dev, 115200)
        self.log = open(log_path, "wb")

    def read(self, seconds: float, until: bytes | None = None,
             echo: bool = False) -> bytes:
        out = b""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            chunk = os.read(self.fd, 512)
            if not chunk:
                continue
            out += chunk
            self.log.write(chunk)
            self.log.flush()
            if echo:
                sys.stdout.buffer.write(chunk.replace(b"\r", b""))
                sys.stdout.flush()
            if until and until in out:
                break
        return out

    def command(self, line: str, seconds: float = 3) -> bytes:
        os.write(self.fd, line.encode() + b"\r")
        return self.read(seconds, until=PROMPT)

    def wait_for_prompt(self) -> bool:
        end = time.monotonic() + PROMPT_WAIT
        while time.monotonic() < end:
            os.write(self.fd, b"\x03")
            if PROMPT in self.read(0.5)[-200:]:
                self.read(1.0)
                return True
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--bootargs", default="")
    ap.add_argument("--log", default="build/serial-boot.log")
    ap.add_argument("--quiet", type=float, default=120.0,
                    help="give up after this many seconds without output")
    args = ap.parse_args()
    with open(args.image, "rb") as f:
        payload = f.read()

    board = Board(args.port, args.log)
    print("serial-boot: waiting for the U-Boot prompt (power-cycle the board if it's stuck)",
          flush=True)
    if not board.wait_for_prompt():
        print("serial-boot: no U-Boot prompt", flush=True)
        return 1

    print(f"serial-boot: sending {args.image} ({len(payload)} bytes)", flush=True)
    os.write(board.fd, f"loadx {LOAD_ADDR}\r".encode())
    board.read(1.0)
    try:
        xmodem.send(board.fd, payload)
    except RuntimeError as e:
        print(f"\nserial-boot: xmodem: {e}", flush=True)
        return 1
    board.read(2.0, until=PROMPT)

    # The PMIC's watchdog has no kernel driver yet, so U-Boot stops it.
    # The SoC's keeps running: the kernel takes it over (plat/ky-x1/wdt.c),
    # so a hung kernel resets the board within a minute.
    for line in ("wdt dev PMIC_WDT", "wdt stop",
                 f"setenv bootargs '{args.bootargs.strip()}'"):
        board.command(line)

    os.write(board.fd, f"booti {LOAD_ADDR} - ${{fdtcontroladdr}}\r".encode())
    out = b""
    last = time.monotonic()
    while time.monotonic() - last < args.quiet:
        chunk = board.read(0.5, echo=True)
        if chunk:
            last = time.monotonic()
            out += chunk
        if b"reboot failed" in out:
            board.read(0.5, echo=True)
            return 1
        if b"planar: hanging" in out:
            # The watchdog should bring the board back within its timeout.
            back = board.read(90.0, until=b"U-Boot", echo=True)
            if b"U-Boot" in back:
                print("\nserial-boot: the board reset itself", flush=True)
                return 0
            print("\nserial-boot: the watchdog didn't bring the board back", flush=True)
            return 1
        if b"planar: rebooting" in out:
            # Success is the board coming back: U-Boot's banner.
            back = board.read(15.0, until=b"U-Boot", echo=True)
            if b"U-Boot" in back:
                print("\nserial-boot: the board reset itself", flush=True)
                return 0
            print("\nserial-boot: the board didn't come back after its reboot", flush=True)
            return 1
        if b"Guru Meditation" in out:
            board.read(2.0, echo=True)
            return 1
    print(f"\nserial-boot: no output for {args.quiet:.0f} s", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
