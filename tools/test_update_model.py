#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
"""Sanity tests for update_model.py against the real dump. Run: python3 -I tools/test_update_model.py"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import update_model as m
def _firmware_image(root):
    """Your own flash dump, firmware/dump.bin (see firmware/README.md)."""
    import os as _os, sys as _sys
    p = _os.path.join(root, "firmware", "dump.bin")
    if not _os.path.exists(p):
        _sys.exit("error: put your own flash dump at firmware/dump.bin first (see firmware/README.md)")
    return open(p, "rb").read()[:0x400000]
flash = _firmware_image(os.path.join(os.path.dirname(__file__), ".."))
def mod(b, off, new):
    b = bytearray(b); b[off:off+len(new)] = new; return bytes(b)
cases = [
  ("identical image",                      flash,                                    0),
  ("checksum dword changed",               mod(flash, 0x10, b"\x01\x00\x00\x00"),    5),
  ("time dword changed",                   mod(flash, 0x14, b"\x01\x00\x00\x00"),    5),
  ("no BLDR",                              mod(flash, 4, b"XXXX"),                  -2),
  ("version-block offset byte differs",    mod(flash, 9, b"\x02"),                   3),
  ("0123/4567 placeholder in file",        mod(flash, 0x10, b"01234567"),            4),
  ("first 4 KiB only, checksum changed",   mod(flash, 0x10, b"\x01\x00\x00\x00")[:0x1000], 5),
  ("too short for version block",          flash[:12],                              -4),
]
bad = 0
for name, f, want in cases:
    st, txt = m.check(flash, f)
    ok = st == want; bad += not ok
    print(("PASS" if ok else "FAIL"), f"{name:42s} -> {st:+d}  {txt}")
new, log = m.simulate_write(flash, cases[6][1])
diff = [i for i in range(len(flash)) if flash[i] != new[i]]
print("4 KiB test file would change", len(diff), "bytes, at", [hex(d) for d in diff])
sys.exit(1 if bad else 0)
