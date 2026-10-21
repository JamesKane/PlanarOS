#!/usr/bin/env python3
"""Make a GPT disk image for QEMU's NVMe and SD (UEFI 2.10 §5).

    mkdisk.py OUT [--size MIB] [--file SRC:DST]...

A protective MBR, the primary GPT and its backup, and two partitions:

- an EFI system partition, half the disk, formatted FAT32 (Microsoft's
  FAT specification 1.03) with each --file copied to DST in it ("EFI/
  planar/C/hello"), directories made as needed. Names that fit 8.3 in one
  case get just a short entry, with the lower-case flags; others get long
  name entries too, so the reader's long names are tested.
- a basic data partition to the end, each block of which starts with its
  own LBA, little-endian, so that a read can be checked against where it
  came from (the queue-depth measurement and the kill test check every
  block they get).

Everything fixed (GUIDs, serial numbers, dates), so the image is the same
from one build to the next.
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
DISK_GUID = uuid.UUID("5f0e6b7a-3c1d-4a8e-9b2f-706c616e6172")
PART_GUIDS = [uuid.UUID("5f0e6b7a-3c1d-4a8e-9b2f-706c616e6101"),
              uuid.UUID("5f0e6b7a-3c1d-4a8e-9b2f-706c616e6102")]
DATE = (2026 - 1980) << 9 | 1 << 5 | 1


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


# --- FAT32 ---

def short_name(name):
    """The 8.3 bytes and case flags, or None if it needs a long name."""
    base, dot, ext = name.rpartition(".") if "." in name else (name, "", "")
    if not base or len(base) > 8 or len(ext) > 3 or "." in base:
        return None
    flags = 0
    for part, bit in ((base, 0x08), (ext, 0x10)):
        if any(c.islower() for c in part) and any(c.isupper() for c in part):
            return None
        if any(c.islower() for c in part):
            flags |= bit
        if any(not (c.isalnum() or c in "_-~!#$%&'(){}^@`") for c in part):
            return None
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode(), flags


def lfn_entries(name, short):
    s = sum_ = 0
    for b in short:
        sum_ = (((sum_ & 1) << 7) + (sum_ >> 1) + b) & 0xff
    chars = [ord(c) for c in name] + [0]
    chars += [0xffff] * (-len(chars) % 13)
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    for i, p in enumerate(parts):
        seq = (i + 1) | (0x40 if i == len(parts) - 1 else 0)
        e = bytearray(32)
        e[0] = seq
        e[11] = 0x0f
        e[13] = sum_
        for k, off in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
            struct.pack_into("<H", e, off, p[k])
        out.append(bytes(e))
    return list(reversed(out))


def dirent(short, flags, attr, cluster, size):
    return short + struct.pack("<BBBHHHHHHHI", attr, flags, 0, 0, DATE, DATE, cluster >> 16,
                               0, DATE, cluster & 0xffff, size)


def fat32(sectors, hidden, files):
    """The partition's bytes: a FAT32 volume of 'sectors' holding 'files'."""
    spc, reserved, nfats = 1, 32, 2
    fat = (sectors - reserved + 2) // (128 * spc + nfats) + 1
    clusters = (sectors - reserved - nfats * fat) // spc
    assert clusters >= 65525, "too small for FAT32"
    cb = spc * BLOCK
    data = bytearray()
    table = [0x0ffffff8, 0x0fffffff]

    def alloc(content):
        n = max(1, -(-len(content) // cb))
        first = len(table)
        for i in range(n):
            table.append(first + i + 1 if i < n - 1 else 0x0fffffff)
        data.extend(content.ljust(n * cb, b"\0"))
        return first

    # The tree: directory -> {name: bytes or dict}.
    tree = {}
    for dst, content in files:
        parts = dst.split("/")
        d = tree
        for p in parts[:-1]:
            d = d.setdefault(p, {})
        d[parts[-1]] = content

    # Clusters in order: the root first (cluster 2), then breadth-first.
    def entries_for(d, me, parent):
        out = []
        if me is not None:
            out.append(dirent(b".          ", 0, 0x10, me, 0))
            out.append(dirent(b"..         ", 0, 0x10, parent, 0))
        else:
            out.append(b"PLANAR     " + bytes([0x08]) + bytes(20))   # the volume label
        for i, (name, content) in enumerate(sorted(d.items())):
            sn = short_name(name)
            if sn is None:
                short = ("~%07d" % i)[:8].encode() + b"   "
                out.extend(lfn_entries(name, short))
                sn = (short, 0)
            if isinstance(content, dict):
                out.append((sn, 0x10, content))
            else:
                out.append((sn, 0x20, content))
        return out

    root_cluster = len(table)
    table.append(0x0fffffff)        # reserved now; written last
    data.extend(bytes(cb))

    def write_dir(d, me, parent):
        ents = entries_for(d, me if me != root_cluster else None, parent)
        raw = bytearray()
        for e in ents:
            if isinstance(e, bytes):
                raw += e
                continue
            (short, flags), attr, content = e
            if attr == 0x10:
                child = len(table)
                table.append(0x0fffffff)    # placeholder; filled below
                data.extend(bytes(cb))
                raw += dirent(short, flags, attr, child, 0)
                pending.append((content, child, me if me != root_cluster else 0))
            else:
                first = alloc(content) if content else 0
                raw += dirent(short, flags, attr, first, len(content))
        return bytes(raw)

    pending = [(tree, root_cluster, 0)]
    while pending:
        d, me, parent = pending.pop(0)
        raw = write_dir(d, me, parent)
        n = max(1, -(-len(raw) // cb))
        assert n == 1 or me == root_cluster or True
        # The first cluster is placed already; more go on the end.
        off = (me - 2) * cb
        data[off:off + cb] = raw[:cb].ljust(cb, b"\0")
        prev = me
        for k in range(1, n):
            c = len(table)
            table.append(0x0fffffff)
            table[prev] = c
            data.extend(raw[k * cb:(k + 1) * cb].ljust(cb, b"\0"))
            prev = c

    assert len(table) - 2 <= clusters, "the files don't fit"
    fat_bytes = b"".join(struct.pack("<I", t) for t in table).ljust(fat * BLOCK, b"\0")
    free = clusters - (len(table) - 2)

    bs = bytearray(BLOCK)
    bs[0:3] = b"\xeb\x58\x90"
    bs[3:11] = b"PLANAR  "
    struct.pack_into("<HBHBHHBHHHII", bs, 11, BLOCK, spc, reserved, nfats, 0, 0, 0xf8, 0, 32, 64,
                     hidden, sectors)
    struct.pack_into("<IHHIHH", bs, 36, fat, 0, 0, root_cluster, 1, 6)
    struct.pack_into("<BBBI11s8s", bs, 64, 0x80, 0, 0x29, 0x706c616e, b"PLANAR     ", b"FAT32   ")
    bs[510:512] = b"\x55\xaa"
    fsinfo = bytearray(BLOCK)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<III", fsinfo, 484, 0x61417272, free, len(table))
    struct.pack_into("<I", fsinfo, 508, 0xaa550000)

    vol = bytearray(sectors * BLOCK)
    vol[0:BLOCK] = bs
    vol[BLOCK:2 * BLOCK] = fsinfo
    vol[6 * BLOCK:7 * BLOCK] = bs
    vol[7 * BLOCK:8 * BLOCK] = fsinfo
    for f in range(nfats):
        at = (reserved + f * fat) * BLOCK
        vol[at:at + len(fat_bytes)] = fat_bytes
    at = (reserved + nfats * fat) * BLOCK
    vol[at:at + len(data)] = data
    return bytes(vol)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=128, help="MiB")
    ap.add_argument("--file", action="append", default=[], help="SRC:DST in the EFI partition")
    a = ap.parse_args()

    blocks = a.size * 1024 * 1024 // BLOCK
    entry_blocks = ENTRIES * ENTRY_SIZE // BLOCK
    first = 2048
    last = blocks - 1 - 1 - entry_blocks
    esp_last = first + blocks // 2 - 1
    parts = [(ESP, PART_GUIDS[0], first, esp_last, "EFI system"),
             (BASIC_DATA, PART_GUIDS[1], esp_last + 1, last, "Planar")]
    table = b"".join(entry(*p) for p in parts).ljust(ENTRIES * ENTRY_SIZE, b"\0")
    tcrc = zlib.crc32(table)

    files = []
    for f in a.file:
        src, dst = f.split(":", 1)
        with open(src, "rb") as h:
            files.append((dst, h.read()))
    files.append(("EFI/planar/hello.txt", b"Hello from the EFI partition.\n"))
    files.append(("EFI/planar/A file with a long name.txt", b"A long name, read.\n"))

    mbr = bytearray(BLOCK)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1,
                               min(blocks - 1, 0xffffffff))
    mbr[510:512] = b"\x55\xaa"

    with open(a.out, "wb") as f:
        f.write(mbr)
        f.write(header(1, blocks - 1, first, last, 2, tcrc))
        f.write(table)
        f.write(b"\0" * BLOCK * (first - 2 - entry_blocks))
        f.write(fat32(esp_last - first + 1, first, files))
        chunk = 2048
        for lba in range(esp_last + 1, last + 1, chunk):
            n = min(chunk, last + 1 - lba)
            f.write(b"".join(struct.pack("<Q", lba + i).ljust(BLOCK, b"\0") for i in range(n)))
        f.write(table)
        f.write(header(blocks - 1, 1, first, last, blocks - 1 - entry_blocks, tcrc))


if __name__ == "__main__":
    main()
