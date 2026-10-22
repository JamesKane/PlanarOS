#!/usr/bin/env python3
"""Run a boot command and check its console transcript.

    boot-check.py --expect 'V0 done' [--timeout 20] [--log FILE]
                  [--forbid REGEX]... [--require-lines FILE]
                  [--facts FILE] [--hwdb-facts FILE] -- CMD...

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
two boots of one machine (make compare-hw). --hwdb-facts writes hwdb's
("[hwdb] hw: " lines, without the "[hwdb] "): what each driver bound,
declined or failed, by its registers (servers/hwdb/server.c).

--qmp PATH is QEMU's QMP socket (-qmp unix:PATH,server=on,wait=off).
A test that wants USB input asks for it on the console, and gets it
through QMP's input-send-event: "inputtest: send keys shift-h i ret"
types those keys (QEMU's qcodes, a "shift-" prefix held over the key),
"inputtest: send mouse 10 -5 left" moves the mouse and clicks a button
(conformance/inputtest.c).
"""

import argparse
import json
import os
import re
import selectors
import socket
import subprocess
import sys
import time

SEND = re.compile(rb"inputtest: send (keys|mouse) ([^\r\n]*)")


class Qmp:
    """QEMU's monitor, for input: connected when first needed."""

    def __init__(self, path):
        self.path = path
        self.sock = None
        self.buf = b""

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise OSError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def _call(self, cmd, args=None):
        msg = {"execute": cmd}
        if args is not None:
            msg["arguments"] = args
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self._line()
            if "return" in r or "error" in r:
                return r

    def _connect(self):
        if self.sock:
            return
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect(self.path)
        self._line()                                    # the greeting
        self._call("qmp_capabilities")

    def _events(self, events):
        self._call("input-send-event", {"events": events})
        time.sleep(0.03)            # a report each: QEMU's keyboard takes one key a poll

    def keys(self, words):
        self._connect()
        for w in words.split():
            held = w.split("-")
            key = lambda k, down: {"type": "key", "data": {
                "down": down, "key": {"type": "qcode", "data": k}}}
            for k in held:
                self._events([key(k, True)])
            for k in reversed(held):
                self._events([key(k, False)])

    def mouse(self, words):
        self._connect()
        dx, dy, button = words.split()
        self._events([{"type": "rel", "data": {"axis": "x", "value": int(dx)}},
                      {"type": "rel", "data": {"axis": "y", "value": int(dy)}}])
        for down in (True, False):
            self._events([{"type": "btn", "data": {"down": down, "button": button}}])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", required=True, help="text that marks a good boot")
    ap.add_argument("--timeout", type=float, default=20.0, help="seconds")
    ap.add_argument("--log", help="also write the transcript here")
    ap.add_argument("--forbid", action="append", default=[],
                    help="a regex no transcript line may match")
    ap.add_argument("--require-lines", help="a file of lines the transcript must have")
    ap.add_argument("--facts", help="write the transcript's 'hw: ' lines here")
    ap.add_argument("--hwdb-facts", help="write hwdb's 'hw: ' lines here")
    ap.add_argument("--qmp", help="QEMU's QMP socket, for typing what a test asks for")
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
    qmp = Qmp(args.qmp) if args.qmp else None
    asked = 0                       # of the transcript, looked at for requests

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
            if qmp:
                done = transcript.rfind(b"\n") + 1
                for m in SEND.finditer(transcript, asked, done):
                    try:
                        (qmp.keys if m.group(1) == b"keys" else qmp.mouse)(m.group(2).decode())
                    except (OSError, ValueError) as e:
                        print(f"\nboot-check: QMP: {e}")
                asked = max(asked, done)
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
    if args.hwdb_facts:
        with open(args.hwdb_facts, "w") as f:
            f.writelines(line[7:] + "\n" for line in lines if line.startswith("[hwdb] hw: "))
    missing = []
    if args.require_lines:
        with open(args.require_lines) as f:
            want = [w.rstrip("\n") for w in f if w.strip() and not w.startswith("#")]
        have = set(lines)
        missing = [w for w in want if w not in have]
    # A test that failed fails the run even when the expected line came
    # after: a board whose 'reboot' resets it within the second given to
    # the rest of the story says "the board reset itself" either way.
    failed = re.search(r" FAIL\b|Guru Meditation", text)
    if found and not bad and not missing and not failed:
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
