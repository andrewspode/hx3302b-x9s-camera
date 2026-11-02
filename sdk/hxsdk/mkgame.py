#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
"""ELF (linked at 0 with hx.ld and -q) -> GAME.BIN: [image incl. zeroed bss][relocation table]; fills the crt0 header.
Table entries (8 bytes): {u32 offset | type << 25 | abs << 31, u32 V} (offset < 32 MiB, type < 64).
Usage: mkgame.py in.elf out.bin. Relocations kept: R_OR1K_32 (1), LO_16_IN_INSN (4), HI_16_IN_INSN (5), AHI16 (35), SLO16 (39) for any target,
and INSN_REL_26 (6) only when the target is a fixed (SHN_ABS) address such as a firmware function; others are rejected."""
import struct, sys
SUPPORTED = {1, 4, 5, 35, 39}
e = open(sys.argv[1], "rb").read()
assert e[:4] == b"\x7fELF" and e[5] == 1, "need a little-endian ELF32"
shoff, = struct.unpack_from("<I", e, 0x20); shentsize, shnum, shstrndx = struct.unpack_from("<HHH", e, 0x2E)
sh = [struct.unpack_from("<10I", e, shoff + i * shentsize) for i in range(shnum)]
strtab_sec = sh[shstrndx]
name = lambda off, sec: e[sec[4] + off: e.index(b"\0", sec[4] + off)].decode()
secs = {name(s[0], strtab_sec): (i, s) for i, s in enumerate(sh)}
symtab = secs[".symtab"][1]; strtab = sh[symtab[6]]
syms = [struct.unpack_from("<IIIBBH", e, symtab[4] + i * 16) for i in range(symtab[5] // 16)]
symname = lambda i: name(syms[i][0], strtab)
end = max(s[3] + s[5] for s in sh if s[2] & 2)                      # highest allocated address (incl. bss)
img = bytearray(end)
for s in sh:
    if s[2] & 2 and s[1] != 8:                                      # SHF_ALLOC, not NOBITS
        img[s[3]:s[3] + s[5]] = e[s[4]:s[4] + s[5]]
rel = []
for nm, (i, s) in secs.items():
    if s[1] != 4: continue                                          # SHT_RELA
    tgt = sh[s[7]]
    if not tgt[2] & 2: continue                                     # relocations of non-loaded sections (debug)
    for k in range(s[5] // 12):
        off, info, add = struct.unpack_from("<IIi", e, s[4] + k * 12)
        typ, si = info & 0xFF, info >> 8
        sv, ssz, sinfo, sother, sndx = syms[si][1], syms[si][2], syms[si][3], syms[si][4], syms[si][5]
        absolute = sndx == 0xFFF1
        V = (sv + add) & 0xFFFFFFFF
        if typ == 6:
            if absolute: rel.append((off | 0x80000000, 6, V))
            continue
        if typ not in SUPPORTED: sys.exit(f"unsupported relocation type {typ} at {off:#x} ({symname(si)})")
        rel.append((off | (0x80000000 if absolute else 0), typ, V))
symval = {symname(i): syms[i][1] for i in range(len(syms))}
bss_s, bss_e = symval["__bss_start"], symval["__bss_end"]
assert struct.unpack_from("<I", img, 4)[0] == 0x4F4C4552, "crt0 header not at offset 4"
assert all((o & 0x7FFFFFFF) < (1 << 25) and t < 64 for o, t, V in rel)
table = b"".join(struct.pack("<II", (o & 0x01FFFFFF) | (t << 25) | (o & 0x80000000), V) for o, t, V in rel)
struct.pack_into("<6I", img, 8, len(img), len(rel), 0, len(img), bss_s, bss_e - bss_s)
open(sys.argv[2], "wb").write(bytes(img) + table)
counts = {}
for r in rel: counts[r[1]] = counts.get(r[1], 0) + 1
print(f"{sys.argv[2]}: image {len(img)} bytes (bss {bss_e - bss_s}), {len(rel)} relocations {counts}, file {len(img) + len(table)} bytes")
