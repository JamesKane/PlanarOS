#!/usr/bin/env python3
"""Boot an image once on a UEFI board that runs FreeBSD, watching its serial console.

    efi-boot.py --host root@BOARD --port /dev/cu.usbserial-X --image build/uefi-a64/planar.bin
                [--bootargs 'layout=global reboot'] [--log FILE]

Over ssh, as root on the board's FreeBSD (docs/v3-plan.md, decision 1):
copies the image to the EFI partition as \\EFI\\planar\\planar.efi, writes
a boot entry for it whose load options are --bootargs (FreeBSD's
efibootmgr can't set load options, so the entry is written with efivar),
sets BootNext to it and reboots. The firmware boots the image once, and
FreeBSD again after it.

What the board sends on the serial port is echoed to stdout, so
tools/boot-check.py can watch it as it watches QEMU, and written to the
log. Exits 0 when the kernel reboots, or hangs (BOOTARGS=hang) and the
watchdog resets the board, and the firmware starts again (TF-A's first
line), or when the stub resets the board (stub=reset); 1 on
a Guru Meditation, a failed reboot, or if the board goes quiet for --quiet
seconds, which needs someone at the power switch. Nothing else may have
the port open while this runs.
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xmodem  # noqa: E402

GLOBAL = "8be4df61-93ca-11d2-aa0d-00e098032b8c"     # EFI_GLOBAL_VARIABLE
ESP_DEV = "/dev/nda0p1"
ESP = "/mnt/esp"
PATH = r"\EFI\planar\planar.efi"
FIRMWARE = b"NOTICE:  BL2:"     # TF-A's first line: the board has reset
ENTRY = "Boot0050"              # ours; well clear of the firmware's own


def ssh(host: str, cmd: str, data: bytes | None = None) -> bytes:
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, cmd],
                       input=data, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"ssh {cmd!r}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout


def hd_node(host: str) -> bytes:
    """The HD() device-path node of the EFI partition, from FreeBSD's own
    boot entry, whose file is on it."""
    for line in ssh(host, "efibootmgr -v").decode().splitlines():
        if "HD(" in line and "freebsd" in line.lower():
            # HD(part,GPT,guid,start,size)
            part, _, guid, start, size = line.split("HD(")[1].split(")")[0].split(",")
            g = guid.split("-")
            gb = (struct.pack("<IHH", int(g[0], 16), int(g[1], 16), int(g[2], 16))
                  + bytes.fromhex(g[3] + g[4]))
            return (struct.pack("<BBHIQQ", 4, 1, 42, int(part), int(start, 16), int(size, 16))
                    + gb + bytes([2, 2]))
    raise RuntimeError("no FreeBSD boot entry to take the EFI partition from")


def load_option(hd: bytes, bootargs: str) -> bytes:
    """An EFI_LOAD_OPTION for the image, with the arguments as its
    optional data: the load options the stub reads."""
    def ucs2(s: str) -> bytes:
        return (s + "\0").encode("utf-16-le")
    fp = ucs2(PATH)
    dp = hd + struct.pack("<BBH", 4, 4, 4 + len(fp)) + fp + bytes([0x7F, 0xFF, 4, 0])
    return struct.pack("<IH", 1, len(dp)) + ucs2("Planar") + dp + bootargs.encode("utf-16-le")


def wait_for_ssh(host: str, seconds: float = 300) -> None:
    """The board may still be coming back from the last run."""
    end = time.monotonic() + seconds
    while True:
        try:
            ssh(host, "true")
            return
        except RuntimeError:
            if time.monotonic() > end:
                raise RuntimeError(f"{host} not back after {seconds:.0f} s")
            time.sleep(5)


def install(host: str, image: bytes, bootargs: str) -> None:
    wait_for_ssh(host)
    opt = load_option(hd_node(host), bootargs)
    ssh(host, f"mkdir -p {ESP} && (mount | grep -q ' {ESP} ' || mount_msdosfs {ESP_DEV} {ESP})"
              f" && mkdir -p {ESP}/EFI/planar && cat > {ESP}/EFI/planar/planar.efi", image)
    # efivar -w takes the value on stdin (its -f isn't implemented for writes).
    ssh(host, f"efivar -w -t 7 -n {GLOBAL}-{ENTRY} && efibootmgr -n -b {ENTRY[4:]} >/dev/null"
              f" && sync && umount {ESP}", opt)
    ssh(host, "shutdown -r now")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--bootargs", default="")
    ap.add_argument("--log", default="build/efi-boot.log")
    ap.add_argument("--quiet", type=float, default=180.0,
                    help="give up after this many seconds without output")
    args = ap.parse_args()
    with open(args.image, "rb") as f:
        image = f.read()

    fd = xmodem.open_port(args.port, 115200)
    log = open(args.log, "wb")
    print(f"efi-boot: installing {args.image} on {args.host}, BootNext, rebooting", flush=True)
    try:
        install(args.host, image, args.bootargs.strip())
    except RuntimeError as e:
        print(f"efi-boot: {e}", flush=True)
        return 1

    out = b""
    last = time.monotonic()
    started = False             # the stub's first line: past the old system
    rebooting = None            # when the kernel said it was rebooting
    while time.monotonic() - last < args.quiet:
        chunk = os.read(fd, 4096)
        if chunk:
            last = time.monotonic()
            log.write(chunk)
            log.flush()
            sys.stdout.buffer.write(chunk.replace(b"\r", b""))
            sys.stdout.flush()
            out += chunk
        if not started:
            started = b"planar: EFI stub" in out
            continue
        if b"planar: EFI stub resetting" in out:
            return 0
        if b"reboot failed" in out or b"Guru Meditation" in out:
            time.sleep(1)
            return 1
        # A reboot should be back in seconds; a hang, when the watchdog
        # resets the board (two of its periods, sbsa_wdt.c).
        for said, wait in ((b"planar: rebooting", 30), (b"planar: hanging", 60)):
            if rebooting is None and said in out:
                rebooting, limit, what = time.monotonic(), wait, said
        if rebooting is not None:
            if FIRMWARE in out.split(what, 1)[1]:
                print("\nefi-boot: the board reset itself", flush=True)
                return 0
            if time.monotonic() - rebooting > limit:
                print("\nefi-boot: the board didn't come back", flush=True)
                return 1
    print(f"\nefi-boot: no output for {args.quiet:.0f} s (power-cycle the board)", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
