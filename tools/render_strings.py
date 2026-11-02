#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
"""Render the camera's UI strings with its own font, one string per row, index in red: python3 -I tools/render_strings.py IMAGE.bin LANG OUT.png
IMAGE is a flash image (the dump or a built update file); LANG 0..17 (0 = English). Format: docs/analysis/resources.md. Needs ImageMagick 'convert'."""
import struct, sys, subprocess, tempfile, os
FONT, FON = 0x24D8EA, 0x25CE4A
d = open(sys.argv[1], "rb").read(); lang = int(sys.argv[2]); out = sys.argv[3]

def glyph(c):
    w, h, off = struct.unpack_from("<HHI", d, FONT + 8 + 8 * c)
    bpr = (w + 7) // 8
    return w, [int.from_bytes(d[FONT + off + y * bpr:FONT + off + (y + 1) * bpr], "big") >> (bpr * 8 - w) for y in range(h)]

blk = FON + struct.unpack_from("<I", d, FON + 12 + 8 * lang)[0]
ns = (struct.unpack_from("<H", d, blk + 14)[0] - 8) // 8
W, rows = 560, []
for s in range(ns):
    w, h, n, off = struct.unpack_from("<HHHH", d, blk + 8 + 8 * s)
    line = [[0] * W for _ in range(24)]; x = 60
    for c in struct.unpack_from("<%dH" % n, d, blk + off):
        if c & 0x8000: c &= 0x3FFF; x -= glyph(c)[0]          # flagged codes (only in Thai, lang 7): GUESS that they overlay the previous glyph; widths say they take no space
        gw, g = glyph(c)
        for y in range(24):
            for k in range(gw):
                if 0 <= x + k < W and (g[y] >> (gw - 1 - k)) & 1: line[y][x + k] = 1
        x += gw
    rows.append(line)
with tempfile.NamedTemporaryFile(suffix=".pbm", delete=False) as f:
    f.write(f"P1 {W} {24 * ns}\n".encode())
    for line in rows:
        for r in line: f.write(("".join("1" if v else "0" for v in r) + "\n").encode())
ann = sum((["-annotate", f"+2+{24 * i + 17}", str(i)] for i in range(ns)), [])
subprocess.run(["convert", f.name, "-fill", "red", "-pointsize", "14", *ann, out], check=True); os.unlink(f.name)
print(f"{ns} strings -> {out}")
