#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Andrew Spode
"""Build a Doom update file (Xly100s.bin) for the HX3302B "X9S" camera from YOUR OWN flash dump, or a file that restores stock firmware.

    python3 -I doom/hxdoom_patch.py build   --dump mydump.bin [-o Xly100s.bin]   [--erase-photos]
    python3 -I doom/hxdoom_patch.py restore --dump mydump.bin [-o Xly100s.bin]
    python3 -I doom/hxdoom_patch.py check   --dump mydump.bin

mydump.bin is the camera's 4 MiB flash, read with tools/usb_flash_read.py or a programmer (a trailing 512 bytes, as a T48 adds, are ignored).
The script never touches the camera or the card: it writes one file, which you copy to the root of the SD card and then power the camera on
normally. The camera flashes it, says "bye bye" and powers off. The output is built from your own dump, so no firmware needs to be shipped with this repository.

What "build" does (see docs/analysis/ and doom/README.md):
  * checks the dump is the stock firmware this was made for (version string `3030B-20A1-20A1 V1.1` at 0xBCB62; sha256 of everything below 0x290000);
  * takes your dump below 0x290000, puts the program loader (doom/release/loader.bin) into the Fly game's slot and rewires Fly's event table;
  * makes the Games button open that loader instead of the game menu (and drops the slide transition);
  * keeps YOUR settings and photo-store header (0x290000-0x292FFF), erases 0x293000-0x29FFFF, and puts Doom (doom/release/DOOM.FLASH.BIN) at 0x2A0000.
    Doom lives in the same flash area as photos taken without an SD card, so those photos are LOST (they can't be copied off over USB).
    The script refuses to continue if it sees JPEG data there, unless you pass --erase-photos;
  * sets the header time to a value different from the installed one (the camera only updates when time or checksum differ; the checksum is left as it is).
You also need DOOM1.WAD (id Software's shareware 1.9) in the card root. Saves are written there too (doomsav0-7.dsg).

"restore" makes a file that flashes your own dump back (everything below 0x290000; settings and photos are not touched).
"""
import argparse, hashlib, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REL = os.path.join(HERE, "release")
FLASH_SIZE = 0x400000
VERSION_AT, VERSION = 0xBCB62, b"3030B-20A1-20A1 V1.1"
STOCK_TIME = 0x668258DC                      # header time (0x14) of the stock build, 2024-07-01
STOCK_SHA256_BELOW_290000 = "0eb3c9f6e97289d622bf478a7e661f2b7368c44f2dc24214def87b2b14b60750"
DOOM_TIME = 0x66BDAC5C                       # header time of the Doom image (stock + 45 days); must differ from the installed time
DOOM_AT, DOOM_END = 0x2A0000, 0x3F0000
DAY = 86400


def w32(b, off, v): struct.pack_into("<I", b, off, v)
def r32(b, off): return struct.unpack_from("<I", b, off)[0]
def movhi(rd, k): return (6 << 26) | (rd << 21) | (k & 0xFFFF)
def ori(rd, ra, k): return (0x2A << 26) | (rd << 21) | (ra << 16) | (k & 0xFFFF)
def pad4k(b): return bytes(b) + b"\xff" * (-len(b) % 0x1000)


def die(msg):
    print("error:", msg, file=sys.stderr)
    sys.exit(1)


def expect(buf, off, word, what):
    if r32(buf, off) != word:
        die(f"{what}: expected {word:#010x} at flash {off:#x}, found {r32(buf, off):#010x}. This is not the firmware this was made for.")


def load_dump(path, force):
    try:
        d = open(path, "rb").read()
    except OSError as e:
        die(f"cannot read {path}: {e.strerror}")
    if len(d) not in (FLASH_SIZE, FLASH_SIZE + 512):
        die(f"{path}: {len(d)} bytes; expected {FLASH_SIZE} (or {FLASH_SIZE + 512} with a T48's trailing bytes)")
    d = d[:FLASH_SIZE]
    if d[4:8] != b"BLDR":
        die("no BLDR header at offset 4: this is not a dump of this camera's flash")
    if d[VERSION_AT:VERSION_AT + len(VERSION)] != VERSION:
        die(f"version string {VERSION.decode()} not found at {VERSION_AT:#x}: a different firmware. Nothing here will work on it.")
    got = hashlib.sha256(d[:0x290000]).hexdigest()
    if got != STOCK_SHA256_BELOW_290000:
        msg = (f"the code area (below 0x290000) is not byte-identical to the stock build (sha256 {got[:16]}... vs {STOCK_SHA256_BELOW_290000[:16]}...). "
               "Typical causes: the camera is not on stock firmware (e.g. it already runs a modified build), or a bad read. ")
        if not force:
            die(msg + "Read it again, or use --force if you know what you are doing (the patch offsets are still verified).")
        print("warning:", msg, file=sys.stderr)
    return bytearray(d)


def find_jpegs(d):
    """Offsets of JPEG start markers in 0x293000-0x3FFFFF (photos taken without an SD card are stored there). A heuristic: a short run of
    bytes can match by chance, but a real photo always starts with it."""
    out, i = [], 0x293000
    while len(out) < 1000:
        i = d.find(b"\xff\xd8\xff", i, FLASH_SIZE)
        if i < 0:
            break
        out.append(i)
        i += 3
    return out


def verify_release():
    """The prebuilt Doom binaries must match the checksums shipped with them: a corrupted file would otherwise be flashed."""
    try:
        sums = [l.split() for l in open(os.path.join(REL, "SHA256SUMS")) if l.strip()]
    except OSError:
        die("doom/release/SHA256SUMS is missing: re-download the repository")
    for want, name in sums:
        try:
            got = hashlib.sha256(open(os.path.join(REL, name), "rb").read()).hexdigest()
        except OSError:
            die(f"doom/release/{name} is missing: re-download the repository")
        if got != want:
            die(f"doom/release/{name} does not match doom/release/SHA256SUMS (corrupted download or a modified file): re-download the repository")


def load_loader():
    code = open(os.path.join(REL, "loader.bin"), "rb").read()
    syms = {l.split()[2]: int(l.split()[0], 16) for l in open(os.path.join(REL, "loader.syms")) if l.strip()}
    return code, syms


def host_image(d, new_time):
    """Flash 0x0-0x4DFFF: the dump with the host loader in Fly's slot and Fly's event table rewired, and the Games button opening it."""
    code, syms = load_loader()
    b = bytearray(d[:0x4E000])
    if not (0x4CBA0 + len(code) <= 0x4D94C):
        die("loader.bin does not fit the Fly slot")
    b[0x4CBA0:0x4CBA0 + len(code)] = code
    orig = {0x09: 0x0604D080, 0x0A: 0x0604CB38, 0x0B: 0x0604CACC, 0x14: 0x0604CBA0, 0x15: 0x0604D910, 0x16: 0x0604D8D4,
            0x17: 0x0604D898, 0x18: 0x0604D85C, 0x19: 0x0604CAF8, 0x11: 0x0604D69C}
    new = {0x09: (0x09, "hx_open"), 0x0A: (0x0A, "hx_close"), 0x0B: (0x0B, "hx_cclose"), 0x14: (0x14, "hx_ok"), 0x15: (0x15, "hx_k15"),
           0x16: (0x16, "hx_k16"), 0x17: (0x17, "hx_k17"), 0x18: (0x18, "hx_k18"), 0x19: (0x19, "hx_back"), 0x11: (0x0D, "hx_frame")}
    o, seen = 0x13134, set()
    while True:
        ev, fn = struct.unpack_from("<II", d, o)
        if ev == 0x1B:
            break
        if orig.get(ev) != fn:
            die(f"Fly's event table: unexpected entry {ev:#x} -> {fn:#x} at {o:#x}")
        nid, sym = new[ev]
        struct.pack_into("<II", b, o, nid, syms[sym])
        seen.add(ev)
        o += 8
    if seen != set(orig):
        die("Fly's event table is incomplete")
    # Games button (mode 7 enter, 0x0604A624..8): open Fly's window (0x02010324 = the host) instead of the game menu window (0x020101D8)
    expect(b, 0x4A620, movhi(3, 0x201), "Games enter: l.movhi r3,0x201")
    expect(b, 0x4A628, ori(3, 3, 0x1D8), "Games enter: l.ori r3,r3,0x1d8")
    w32(b, 0x4A628, ori(3, 3, 0x324))
    # drop the slide transition into the game menu (0x0604A608: l.jal 0x060518B8 -> l.nop)
    pc = 0x4A608
    expect(b, pc, (0x01 << 26) | (((0x060518B8 - 0x0604A608) >> 2) & 0x3FFFFFF), "Games enter: l.jal (transition request)")
    w32(b, pc, 0x15000000)
    w32(b, 0x14, new_time)
    return b


def new_header_time(d):
    t = r32(d, 0x14)
    return DOOM_TIME if t != DOOM_TIME else DOOM_TIME + DAY


def cmd_check(a):
    d = load_dump(a.dump, a.force)
    jp = find_jpegs(d)
    print("OK: this is the stock firmware (version string and code area match). `build` can make a Doom update from it.")
    print("  header time", hex(r32(d, 0x14)), "checksum", hex(r32(d, 0x10)))
    print("  settings block at 0x290000, first bytes", d[0x290000:0x290010].hex())
    if jp:
        print(f"  photos: {len(jp)} JPEG start marker(s) found in the internal photo store (first at {jp[0]:#x}). `build` will ask you to confirm")
        print("  with --erase-photos, because Doom is written over that area and photos taken without an SD card are lost.")
    else:
        print("  photos: none found in the internal photo store, so nothing there will be lost.")


def cmd_build(a):
    verify_release()
    d = load_dump(a.dump, a.force)
    jp = find_jpegs(d)
    if jp and not a.erase_photos:
        die(f"the camera's internal photo store (flash 0x291000 onwards) looks as if it holds photos ({len(jp)} JPEG marker(s), first at {jp[0]:#x}).\n"
            "       Doom is written into that area, so they WOULD BE LOST: they cannot be copied off over USB. (The raw data stays in your dump.)\n"
            "       If you do not need them, run again with --erase-photos.")
    prog = open(os.path.join(REL, "DOOM.FLASH.BIN"), "rb").read()
    if prog[:4] != b"HXDM" or DOOM_AT + len(prog) > DOOM_END:
        die("DOOM.FLASH.BIN is not a Doom image or is too large")
    if d[0x291003:0x29100B] != b"MSDOS5.0":
        print("note: the internal photo store has no photo volume header (normal for a camera that has never saved a photo without a card). Continuing.",
              file=sys.stderr)
    host = host_image(d, new_header_time(d))
    out = bytearray(host) + d[0x4E000:0x290000] + d[0x290000:0x293000] + b"\xff" * (DOOM_AT - 0x293000) + prog
    out = pad4k(out)
    open(a.output, "wb").write(out)
    print(f"wrote {a.output}: {len(out):#x} bytes, header time {r32(out, 0x14):#x} (installed {r32(d, 0x14):#x}), sha256 {hashlib.sha256(out).hexdigest()}")
    print("Next: copy it to the root of the SD card named exactly Xly100s.bin, together with DOOM1.WAD. Eject the card, unplug the USB cable and")
    print("make sure the battery is well charged, then press the power button. You should see a progress bar and then 'bye bye'; do not touch")
    print("the camera until it has switched itself off. Then switch it on and open Games.")
    print("Keep your dump: it is your recovery image (see `restore`).")


def cmd_restore(a):
    d = load_dump(a.dump, a.force)
    b = bytearray(d[:0x290000])                      # settings and photos are left alone
    t = r32(b, 0x14)
    if t == DOOM_TIME:
        die("this dump already carries the Doom image's header time; restore from a dump of the stock firmware")
    open(a.output, "wb").write(pad4k(b))
    print(f"wrote {a.output}: {len(b):#x} bytes, header time {t:#x} (differs from Doom's {DOOM_TIME:#x}, so the camera accepts it)")
    print("Next: copy it to the root of the SD card named exactly Xly100s.bin, eject the card, unplug the USB cable and press the power button (battery well charged).")
    print("It restores the firmware below 0x290000 only: your settings are not touched and photos that Doom overwrote do not come back.")
    print("If the camera is already running the stock firmware it will quietly ignore this file (nothing happens, no error).")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["build", "restore", "check"])
    ap.add_argument("--dump", required=True, help="your 4 MiB flash dump")
    ap.add_argument("-o", "--output", default="Xly100s.bin")
    ap.add_argument("--erase-photos", action="store_true", help="acknowledge that photos in the internal store will be lost")
    ap.add_argument("--force", action="store_true", help="continue although the code area is not the stock build. DANGEROUS: it can produce a file that bricks the camera")
    a = ap.parse_args()
    {"build": cmd_build, "restore": cmd_restore, "check": cmd_check}[a.command](a)


if __name__ == "__main__":
    main()
