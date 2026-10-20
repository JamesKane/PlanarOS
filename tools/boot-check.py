#!/usr/bin/env python3
"""Run a boot command and check its console transcript.

    boot-check.py --expect 'V0 done' [--timeout 20] [--log FILE] -- CMD...

Starts CMD (QEMU, or the hosted executable), echoes its output, and
succeeds as soon as a line containing the expected text appears. Fails
if the command exits or the timeout passes first. The kernel halts rather
than powering off, so QEMU is killed once the transcript is complete.
"""

import argparse
import os
import selectors
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", required=True, help="text that marks a good boot")
    ap.add_argument("--timeout", type=float, default=20.0, help="seconds")
    ap.add_argument("--log", help="also write the transcript here")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    if not cmd:
        ap.error("no command given")

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + args.timeout
    transcript = b""
    found = False

    try:
        while not found:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            if not sel.select(timeout=left):
                continue
            chunk = os.read(proc.stdout.fileno(), 4096)
            if not chunk:
                break
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
            transcript += chunk
            found = args.expect.encode() in transcript
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()

    if args.log:
        with open(args.log, "wb") as f:
            f.write(transcript)
    if found:
        print(f"\nboot-check: ok ('{args.expect}')")
        return 0
    why = "timed out" if time.monotonic() >= deadline else f"exited with {proc.returncode}"
    print(f"\nboot-check: FAILED: {why} before '{args.expect}'")
    return 1


if __name__ == "__main__":
    sys.exit(main())
