#!/usr/bin/env python3
"""Rebuild a device tree blob from a boot transcript.

    tools/dtb-from-log.py LOG OUT.dtb

A kernel booted with the argument "dumpdtb" prints the tree the firmware
gave it as "dtb: " lines of hex (kernel/fdt.c, fdt_dump_blob), ending with
"dtb: end, N bytes of M", leaving out the padding after the strings.
This joins them, checks the length and the magic, and sets the header's
size to the bytes kept.
"""
import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    data, size = bytearray(), None
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            m = re.search(r"dtb: end, (\d+) bytes", line)
            if m:
                size = int(m.group(1))
                break
            m = re.search(r"dtb: ([0-9a-f]+)\s*$", line)
            if m:
                data += bytes.fromhex(m.group(1))
    if size is None:
        sys.exit("dtb-from-log: no \"dtb: end\" line")
    if len(data) != size:
        sys.exit(f"dtb-from-log: {len(data)} bytes, but the kernel said {size}")
    if data[:4] != b"\xd0\x0d\xfe\xed":
        sys.exit("dtb-from-log: not a device tree (bad magic)")
    data[4:8] = size.to_bytes(4, "big")
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print(f"dtb-from-log: {size} bytes to {sys.argv[2]}")


if __name__ == "__main__":
    main()
