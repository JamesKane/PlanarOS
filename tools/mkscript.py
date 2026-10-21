#!/usr/bin/env python3
"""Wrap a U-Boot command file as a legacy script image (boot.scr).

Does what `mkimage -A riscv -T script -C none -n NAME -d IN OUT` does, so
building the SD card needs no u-boot-tools.

    tools/mkscript.py plat/ky-x1/boot.cmd build/ky-x1/boot.scr

From omega (tools/mkscript.py there).
"""
import struct
import sys
import zlib

IH_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_RISCV = 26
IH_TYPE_SCRIPT = 6
IH_COMP_NONE = 0


def script_image(script: bytes, name: bytes = b"planar") -> bytes:
    # A script image's payload is a zero-terminated list of part lengths,
    # then the parts: here a single part, the script text.
    data = struct.pack(">II", len(script), 0) + script
    header = struct.pack(
        ">IIIIIIIBBBB32s",
        IH_MAGIC, 0, 0, len(data), 0, 0,
        zlib.crc32(data), IH_OS_LINUX, IH_ARCH_RISCV, IH_TYPE_SCRIPT,
        IH_COMP_NONE, name)
    hcrc = zlib.crc32(header)
    return header[:4] + struct.pack(">I", hcrc) + header[8:] + data


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        script = f.read()
    with open(sys.argv[2], "wb") as f:
        f.write(script_image(script))
    return 0


if __name__ == "__main__":
    sys.exit(main())
