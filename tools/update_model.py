#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
"""Offline model of the camera's SD-card update, written from the disassembly of the routine at flash 0x4900-0x53F0
(docs/analysis/update-routine.md). It models only the check, not the writer. Files whose header time or checksum differs are accepted on the camera, as predicted; the refusal of an identical file cannot be seen from outside, so that half is inferred.

    update_model.py check FLASH.bin UPDATE.bin     # would the check accept it, and why?
    update_model.py write FLASH.bin UPDATE.bin     # also simulate the erase/program sequence and print what changes
"""
import struct, sys

BLDR = b"BLDR"
PLACEHOLDER = (b"0123", b"4567")          # ASCII placeholders the firmware treats as "unversioned"
SECTOR = 0x1000                           # erase unit (SPI opcode 0x20); program unit is 256-byte pages (0x02)

def u32(b, o):
    return struct.unpack_from("<I", b, o)[0] if o + 4 <= len(b) else None

def check(flash: bytes, binf: bytes):
    """Return (status, text). status > 0 -> the firmware goes on to flash; <= 0 -> it refuses."""
    # 1. installed firmware header, read from flash
    if flash[4:8] != BLDR:
        return 1, "installed flash has no BLDR header -> update proceeds (recovery path)"
    mem = flash[9] << 4
    cur_ck, cur_t = u32(flash, mem), u32(flash, mem + 4)
    if (flash[mem:mem+4], flash[mem+4:mem+8]) == PLACEHOLDER:
        return 2, "installed version block is the 0123/4567 placeholder -> update proceeds"
    # 2. the SD file
    if len(binf) < 8 or binf[4:8] != BLDR:
        return -2, "file has no BLDR at offset 4 -> refused"
    if len(binf) < 10:
        return -3, "file too short to hold the version-block offset byte -> refused"
    bmem = binf[9] << 4
    if bmem != mem:
        return 3, f"version-block offset differs (file {bmem:#x}, installed {mem:#x}) -> update proceeds"
    bin_ck, bin_t = u32(binf, bmem), u32(binf, bmem + 4)
    if bin_ck is None or bin_t is None:
        return -4, "file too short to hold the version block -> refused"
    if (binf[bmem:bmem+4], binf[bmem+4:bmem+8]) == PLACEHOLDER:
        return 4, "file carries the 0123/4567 placeholder -> update proceeds (force)"
    if bin_t != cur_t or bin_ck != cur_ck:
        return 5, (f"version differs (installed ck={cur_ck:#x} t={cur_t:#x}; file ck={bin_ck:#x} t={bin_t:#x})"
                   " -> update proceeds")
    return 0, "checksum and time identical -> 'check same ,no to update'"

def simulate_write(flash: bytes, binf: bytes):
    """Return (new_flash, log). Order from the loop at 0x51B4: 4 KiB at a time from offset 0, with the first 512 bytes
    zeroed on the first pass; then chunk 0 is re-read and written again with the real header. File size, not flash size,
    bounds the loop. ASSUMPTION: a short last chunk is padded with 0xFF here; the real buffer's tail content is unknown."""
    out = bytearray(flash); log = []
    size = len(binf)
    nchunks = (size + SECTOR - 1) // SECTOR
    def chunk(i):
        c = bytearray(binf[i*SECTOR:(i+1)*SECTOR]); c += b"\xff" * (SECTOR - len(c)); return c
    for i in range(nchunks):
        c = chunk(i)
        if i == 0:
            c[:0x200] = b"\x00" * 0x200
            log.append("pass 1, chunk 0: first 512 bytes (header) written as ZEROS")
        out[i*SECTOR:(i+1)*SECTOR] = c
    log.append(f"pass 1: sectors 0..{nchunks-1} written ({nchunks*SECTOR:#x} bytes)")
    out[0:SECTOR] = chunk(0)
    log.append("pass 2: sector 0 rewritten with the real header from the file")
    return bytes(out), log

def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("check", "write"):
        sys.exit(__doc__)
    flash = open(sys.argv[2], "rb").read()[:0x400000]
    binf = open(sys.argv[3], "rb").read()
    st, txt = check(flash, binf)
    print(f"status {st:+d}: {txt}")
    if sys.argv[1] == "write" and st > 0:
        new, log = simulate_write(flash, binf)
        for l in log: print(" ", l)
        diff = [i for i in range(len(flash)) if flash[i] != new[i]]
        print(f"  bytes that would change: {len(diff)}" + (f" (first {diff[0]:#x}, last {diff[-1]:#x})" if diff else ""))

if __name__ == "__main__":
    main()
