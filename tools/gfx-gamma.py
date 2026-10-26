#!/usr/bin/env python3
"""tools/gfx-gamma.py -- lib/gfx/gamma.c: sRGB to linear light and back

gfx.library blends coverage in linear light (docs §14.3.4): a byte of
sRGB to 16 bits of linear by a 256-entry table, and back by a
4,096-entry one, indexed by the linear value's top 12 bits, each entry
the sRGB of its bin's middle. Run it to write lib/gfx/gamma.c; it checks
that every byte comes back as itself.
"""
import sys


def to_linear(v):
    c = v / 255
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def to_srgb(lin):
    c = 12.92 * lin if lin <= 0.0031308 else 1.055 * lin ** (1 / 2.4) - 0.055
    return min(255, max(0, round(c * 255)))


lin = [round(to_linear(v) * 65535) for v in range(256)]
enc = [to_srgb((i * 16 + 8) / 65535) for i in range(4096)]
for v in range(256):
    assert enc[lin[v] >> 4] == v, v

out = ["/*",
       " * lib/gfx/gamma.c -- sRGB to linear light and back (V8 step 3)",
       " *",
       " * Written by tools/gfx-gamma.py: don't edit. gfx_linear is a byte of",
       " * sRGB as 16 bits of linear light; gfx_srgb a linear value's top 12",
       " * bits as sRGB, the middle of its bin. Every byte comes back as itself.",
       " */",
       '#include "rows.h"',
       "",
       "const uint16_t gfx_linear[256] = {"]
for i in range(0, 256, 8):
    out.append("    " + " ".join(f"{v}," for v in lin[i:i + 8]))
out += ["};", "", "const uint8_t gfx_srgb[4096] = {"]
for i in range(0, 4096, 16):
    out.append("    " + " ".join(f"{v}," for v in enc[i:i + 16]))
out.append("};")
open(sys.argv[1] if len(sys.argv) > 1 else "lib/gfx/gamma.c", "w").write("\n".join(out) + "\n")
