#!/usr/bin/env python3
"""Rebuild ACPI tables from a boot transcript.

    tools/acpi-from-log.py LOG OUTDIR

A kernel booted with the argument "dumpacpi" prints every table it read
(kernel/acpi.c, acpi_dump_tables) as "acpi: " lines of hex, each table
ending with "acpi: end SIG, N bytes". This writes each as OUTDIR/SIG.dat,
a second of the same signature as SIG2.dat and so on (as the CIX's dumps
are named), checking each length and checksum.
"""
import os
import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    out, data, seen = sys.argv[2], bytearray(), {}
    os.makedirs(out, exist_ok=True)
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            m = re.search(r"acpi: end (....), (\d+) bytes", line)
            if m:
                sig, size = m.group(1), int(m.group(2))
                if len(data) != size or data[:4].decode("latin-1") != sig:
                    sys.exit(f"acpi-from-log: {sig}: {len(data)} bytes, the kernel said {size}")
                if sum(data) % 256:
                    sys.exit(f"acpi-from-log: {sig}: bad checksum")
                seen[sig] = seen.get(sig, 0) + 1
                name = sig + (str(seen[sig]) if seen[sig] > 1 else "") + ".dat"
                with open(os.path.join(out, name), "wb") as t:
                    t.write(data)
                print(f"acpi-from-log: {name}, {size} bytes")
                data = bytearray()
                continue
            m = re.search(r"acpi: ([0-9a-f]+)\s*$", line)
            if m:
                data += bytes.fromhex(m.group(1))
    if not seen:
        sys.exit('acpi-from-log: no "acpi: end" line')


if __name__ == "__main__":
    main()
