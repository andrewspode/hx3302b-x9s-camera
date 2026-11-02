#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
"""Make a copy of the flash dump that is safe to publish: the camera's internal photo store (flash 0x291000-0x3FFFFF, photos taken
without an SD card) is replaced by 0xFF (erased flash), and the 512 trailing bytes of unknown origin are dropped.
Usage: python3 -I tools/make_public_dump.py OUT.bin   (reads firmware/dump.bin; writes nothing else)
Check before publishing: no JPEG may remain at or above 0x290000 (the script asserts this)."""
import sys, os, hashlib
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
d = bytearray(open(os.path.join(ROOT, "firmware", "dump.bin"), "rb").read()[:0x400000])
STORE = 0x291000                     # 'MSDOS5.0'-signed header of the photo store; 0x16F (367) 4 KiB sectors to the end of flash
assert d[STORE + 3:STORE + 11] == b"MSDOS5.0"
d[STORE:] = b"\xff" * (len(d) - STORE)
assert d.find(b"\xff\xd8\xff", 0x290000) < 0
open(sys.argv[1], "wb").write(d)
print(sys.argv[1], len(d), hashlib.sha256(d).hexdigest())
