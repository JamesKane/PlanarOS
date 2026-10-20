#!/usr/bin/env python3
"""Run a boot command and check its console transcript.

    boot-check.py --expect 'V0 done' [--timeout 20] [--log FILE]
                  [--forbid REGEX]... [--require-lines FILE]
                  [--facts FILE] -- CMD...

Starts CMD (QEMU, or the hosted executable), echoes its output, and
succeeds as soon as a line containing the expected text appears. Fails
if the command exits or the timeout passes first, at once if a test says
FAIL or the kernel meditates, and at the end if any line matches a
--forbid pattern (a platform that must run every test forbids
"skipped"), or if a line of the --require-lines file (blank lines and
'#' comments aside) isn't in the transcript, whole. The kernel halts
rather than powering off, so QEMU is killed once the transcript is
complete.

--facts writes the transcript's "hw: " lines to FILE: the facts the
kernel took from its firmware description (kernel/hw.h), for comparing
two boots of one machine (make compare-hw).
"""

import argparse
import os
import re
import selectors
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", required=True, help="text that marks a good boot")
    ap.add_argument("--timeout", type=float, default=20.0, help="seconds")
    ap.add_argument("--log", help="also write the transcript here")
    ap.add_argument("--forbid", action="append", default=[],
                    help="a regex no transcript line may match")
    ap.add_argument("--require-lines", help="a file of lines the transcript must have")
    ap.add_argument("--facts", help="write the transcript's 'hw: ' lines here")
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
            if re.search(rb" FAIL\b|Guru Meditation", transcript):
                deadline = min(deadline, time.monotonic() + 1.0)     # the rest of the story
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()

    if args.log:
        with open(args.log, "wb") as f:
            f.write(transcript)
    text = transcript.decode(errors="replace")
    lines = [line.rstrip("\r") for line in text.splitlines()]
    bad = [line for line in lines for pat in args.forbid if re.search(pat, line)]
    if args.facts:
        with open(args.facts, "w") as f:
            f.writelines(line + "\n" for line in lines if line.startswith("hw: "))
    missing = []
    if args.require_lines:
        with open(args.require_lines) as f:
            want = [w.rstrip("\n") for w in f if w.strip() and not w.startswith("#")]
        have = set(lines)
        missing = [w for w in want if w not in have]
    if found and not bad and not missing:
        print(f"\nboot-check: ok ('{args.expect}')")
        return 0
    if bad:
        print(f"\nboot-check: FAILED: forbidden line: {bad[0].strip()}")
        return 1
    if found and missing:
        print(f"\nboot-check: FAILED: no line '{missing[0]}' ({len(missing)} missing)")
        return 1
    if re.search(r" FAIL\b|Guru Meditation", text):
        why = "a test failed"
    else:
        why = "timed out" if time.monotonic() >= deadline else f"exited with {proc.returncode}"
    print(f"\nboot-check: FAILED: {why} before '{args.expect}'")
    return 1


if __name__ == "__main__":
    sys.exit(main())
