#!/usr/bin/env python3
"""Make a GPT disk image for QEMU's NVMe and SD (UEFI 2.10 §5).

    mkdisk.py OUT [--size MIB]

A protective MBR, the primary GPT and its backup, and two partitions:
an EFI system partition, then a basic data one to the end. Each block of
the partitions starts with its own LBA, little-endian, so that a read
can be checked against where it came from (the queue-depth measurement
reads at random and checks every block it gets).
"""
import argparse
import struct
import uuid
import zlib

BLOCK = 512
ENTRIES = 128
ENTRY_SIZE = 128
ESP = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
BASIC_DATA = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
# Fixed, so the image is the same from one build to the next.
DISK_GUID = uuid.UUID("5f0e6b7a-3c1d-4a8e-9b2f-706c616e6172")
PART_GUIDS = [uuid.UUID("5f0e6b7a-3c1d-4a8e-9b2f-706c616e6101"),
              uuid.UUID("5f0e6b7a-3c1d-4a8e-9b2f-706c616e6102")]


def header(lba, backup, first, last, entries_lba, entries_crc):
    h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, lba, backup,
                    first, last, DISK_GUID.bytes_le, entries_lba, ENTRIES, ENTRY_SIZE,
                    entries_crc)
    crc = zlib.crc32(h)
    h = h[:16] + struct.pack("<I", crc) + h[20:]
    return h.ljust(BLOCK, b"\0")


def entry(type_guid, part_guid, first, last, name):
    n = name.encode("utf-16-le").ljust(72, b"\0")
    return struct.pack("<16s16sQQQ72s", type_guid.bytes_le, part_guid.bytes_le, first, last,
                       0, n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=64, help="MiB")
    a = ap.parse_args()

    blocks = a.size * 1024 * 1024 // BLOCK
    entry_blocks = ENTRIES * ENTRY_SIZE // BLOCK
    first = 2048
    last = blocks - 1 - 1 - entry_blocks
    esp_last = first + 32 * 2048 - 1 if blocks > 128 * 2048 else first + (last - first) // 2
    parts = [(ESP, PART_GUIDS[0], first, esp_last, "EFI system"),
             (BASIC_DATA, PART_GUIDS[1], esp_last + 1, last, "Planar")]
    table = b"".join(entry(*p) for p in parts).ljust(ENTRIES * ENTRY_SIZE, b"\0")
    tcrc = zlib.crc32(table)

    mbr = bytearray(BLOCK)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1,
                               min(blocks - 1, 0xffffffff))
    mbr[510:512] = b"\x55\xaa"

    with open(a.out, "wb") as f:
        f.write(mbr)
        f.write(header(1, blocks - 1, first, last, 2, tcrc))
        f.write(table)
        f.write(b"\0" * BLOCK * (first - 2 - entry_blocks))
        chunk = 2048
        for lba in range(first, last + 1, chunk):
            n = min(chunk, last + 1 - lba)
            f.write(b"".join(struct.pack("<Q", lba + i).ljust(BLOCK, b"\0") for i in range(n)))
        f.write(table)
        f.write(header(blocks - 1, 1, first, last, blocks - 1 - entry_blocks, tcrc))


if __name__ == "__main__":
    main()
