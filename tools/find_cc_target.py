#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
# Find what the USB vendor command 0xCC was built to call.
#
# 0xCC (handler flash 0x24B10) calls an address the host supplies in CDB[1..4]
# with r3 = &usbstruct+0x30 (parsed command block) and r4 = &usbstruct+0x74
# (buffer). The intended target must read a source address out of the command
# block and send memory to the host. The only "send bytes to host" primitives
# are flash 0x246E8 (helper, sends via the leaf) and RAM 0x02000574 (leaf).
# So: scan the whole image for OR1K l.j/l.jal, re-base each instruction to its
# TRUE runtime address (the RAM-copy region flash 0x2E00-0x14000 runs at
# 0x02000000+, the big body flash 0x14000-0x7E000 runs in place at 0x06000000+,
# first stage flash 0x200-0x2E00 lives in SRAM at 0x0+), and list every caller
# of those two send primitives together with its containing function.
#
# Pure stdlib; run: python3 -I tools/find_cc_target.py
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMP = os.path.join(REPO, "firmware", "dump.bin")
if not os.path.exists(DUMP): DUMP = os.path.join(REPO, "firmware", "HX3302B_3030B-20A1-20A1_V1.1_20240701.bin")   # (identical below 0x291000)
SYMS = os.path.join(REPO, "config", "symbols.txt")

RAMCOPY_LO, RAMCOPY_HI = 0x2E00, 0x14000   # -> 0x02000000 +
BODY_LO, BODY_HI = 0x14000, 0x7E000        # in place at 0x06000000 +
SRAM_LO, SRAM_HI = 0x200, 0x2E00           # -> address 0 +

SEND_LEAF = 0x02000574    # runtime addr (flash 0x3374): bulk-IN send(ptr,count)
SEND_HELP = 0x060246E8    # in-place body: send buffer +0x7C via the leaf
CSW_BUILD = 0x06024B5C    # CSW sender


def runtime_pc(off):
    if RAMCOPY_LO <= off < RAMCOPY_HI:
        return 0x02000000 + (off - RAMCOPY_LO)
    if BODY_LO <= off < BODY_HI:
        return 0x06000000 + off
    if SRAM_LO <= off < SRAM_HI:
        return off - SRAM_LO
    return None


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def load_functions():
    fns = []  # (flash_start, size, name)
    with open(SYMS) as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.split(";")[0].split()
            if len(parts) < 4:
                continue
            addr, _run, size, name = parts[0], parts[1], parts[2], parts[3]
            a = int(addr, 16)
            off = a - 0x06000000 if a >= 0x06000000 else a  # symbols.txt is import-base based
            fns.append((off, int(size, 16), name))
    fns.sort()
    return fns


def enclosing(fns, off):
    lo, hi = 0, len(fns) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        start, size, name = fns[mid]
        if start <= off < start + max(size, 1):
            return name, start
        if start < off:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    if best is not None:
        start, size, name = fns[best]
        return name + " (past end)", start
    return "?", None


def main():
    with open(DUMP, "rb") as f:
        img = f.read(4 * 1024 * 1024)
    fns = load_functions()
    hits = {SEND_LEAF: [], SEND_HELP: [], CSW_BUILD: []}
    off = 0
    while off + 4 <= len(img):
        word = int.from_bytes(img[off:off + 4], "little")
        op = word >> 26
        if op in (0x00, 0x01):  # l.j / l.jal
            pc = runtime_pc(off)
            if pc is not None:
                disp = sext(word & 0x3FFFFFF, 26) << 2
                tgt = pc + disp
                if tgt in hits:
                    fn, fstart = enclosing(fns, off)
                    hits[tgt].append((off, pc, op, fn, fstart))
        off += 4
    names = {SEND_LEAF: "send leaf 0x02000574", SEND_HELP: "send helper 0x060246E8",
             CSW_BUILD: "CSW builder 0x06024B5C"}
    for tgt in (SEND_LEAF, SEND_HELP, CSW_BUILD):
        print("=== callers of %s ===" % names[tgt])
        seen = set()
        for off, pc, op, fn, fstart in sorted(hits[tgt], key=lambda h: h[0]):
            key = (fn, fstart)
            tag = "" if key not in seen else "   (same fn)"
            seen.add(key)
            print("  call @flash 0x%06X (pc 0x%08X, %s)  in %s @0x%06X%s"
                  % (off, pc, "jal" if op == 1 else "j ", fn, fstart or 0, tag))
        print()
    print("total distinct functions calling leaf+helper: %d"
          % len({(h[3], h[4]) for t in (SEND_LEAF, SEND_HELP) for h in hits[t]}))


if __name__ == "__main__":
    sys.exit(main())
