#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
"""Offline check of a GAME.BIN's relocation: apply its table exactly as crt0 does, for base B, and compare with the same program linked
directly at B (allocated sections). Header words 8..31 (filled by mkgame) are excluded. Usage: relocheck.py GAME.BIN linked_at_B.elf B"""
import struct, sys
g = bytearray(open(sys.argv[1], "rb").read()); B = int(sys.argv[3], 0)
off, cnt, done, size, bss_s, bss_n = struct.unpack_from("<6I", g, 8)
img = bytearray(g[:size])
for i in range(cnt):
    o, V = struct.unpack_from("<II", g, off + 8 * i)
    a = o & 0x01FFFFFF; t = (o >> 25) & 0x3F; A = (V + (0 if o >> 31 else B)) & 0xFFFFFFFF
    w, = struct.unpack_from("<I", img, a)
    if t == 1: w = A
    elif t == 4: w = (w & 0xFFFF0000) | (A & 0xFFFF)
    elif t == 5: w = (w & 0xFFFF0000) | (A >> 16)
    elif t == 35: w = (w & 0xFFFF0000) | (((A + 0x8000) >> 16) & 0xFFFF)
    elif t == 39: w = (w & ~((0x1F << 21) | 0x7FF) & 0xFFFFFFFF) | (((A >> 11) & 0x1F) << 21) | (A & 0x7FF)
    elif t == 6: w = (w & 0xFC000000) | (((A - (B + a)) >> 2) & 0x03FFFFFF)
    struct.pack_into("<I", img, a, w)
e = open(sys.argv[2], "rb").read()
shoff, = struct.unpack_from("<I", e, 0x20); shentsize, shnum, _ = struct.unpack_from("<HHH", e, 0x2E)
ref = bytearray(size)
for i in range(shnum):
    s = struct.unpack_from("<10I", e, shoff + i * shentsize)
    if s[2] & 2 and s[1] != 8: ref[s[3] - B:s[3] - B + s[5]] = e[s[4]:s[4] + s[5]]
bad = [i for i in range(size) if img[i] != ref[i] and not 8 <= i < 32]
print(f"relocheck at {B:#x}: {cnt} relocations applied, {len(bad)} differing bytes" + (f" e.g. at {[hex(x) for x in bad[:8]]}" if bad else " -> OK"))
sys.exit(1 if bad else 0)
