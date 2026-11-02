#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
"""Tiny OpenRISC (env EMU_DIR=folder: files are read from / written to that host folder; EMU_LIMIT=max instructions per call;
 EMU_PROFILE=file: count executed instructions per address of the program image, separately for INIT and for FRAME calls, saved as
 'EMUP', u32 load address, u32 words, then two u32[words] count arrays (init, frames);
 EMU_FLASH=1: the program file is a flash-program image (HXDM header) mapped read-only at 0x062A0000; it is started like the host does:
 copy the RAM parts, zero the bss ranges, call the header's entry)
 (little-endian, NO delay slots: l.jal sets r9 = pc + 4) emulator for testing GAME.BIN programs offline.
Firmware functions are Python stubs at their real addresses; the services table is built in emulated RAM like the host's.
Usage: emu.py GAME.BIN [frames]   (loads at 0x02112C40, the address malloc gave on the camera; prints a trace of firmware calls)"""
import struct, sys
RAM0, RAMN = 0x02000000, 0x200000
class Mem:
    def __init__(s): s.ram = bytearray(RAMN); s.sram = bytearray(0x10000); s.flash = None
    def buf(s, a, n):
        if RAM0 <= a and a + n <= RAM0 + RAMN: return s.ram, a - RAM0
        if s.flash is not None and 0x06000000 <= a and a + n <= 0x06400000: return s.flash, a - 0x06000000
        if 0 <= a and a + n <= 0x10000: return s.sram, a
        raise MemoryError(f"bad access {a:#010x}")
    def r(s, a, n): b, o = s.buf(a, n); return int.from_bytes(b[o:o + n], "little")
    def w(s, a, n, v): b, o = s.buf(a, n); b[o:o + n] = (v & ((1 << 8 * n) - 1)).to_bytes(n, "little")
sx = lambda v, bits: v - (1 << bits) if v & (1 << (bits - 1)) else v
M32 = 0xFFFFFFFF

class CPU:
    def __init__(s, mem, stubs):
        s.m, s.r, s.f, s.stubs, s.steps = mem, [0] * 32, False, stubs, 0
        s.prof = None                                                   # (array, base, words) while profiling
    def call(s, pc, args, ret=0xDEAD0000, limit=50_000_000):
        s.steps = 0
        s.r[3:3 + len(args)] = args; s.r[9] = ret; s.pc = pc
        while s.pc != ret:
            if s.pc in s.stubs:
                s.r[11] = s.stubs[s.pc](s) or 0; s.pc = s.r[9]; continue
            if s.prof is not None:
                k = (s.pc - s.prof[1]) >> 2
                if 0 <= k < s.prof[2]: s.prof[0][k] += 1
            s.step(); s.steps += 1
            if s.steps > limit: raise RuntimeError(f"step limit at pc {s.pc:#010x}")
        return s.r[11]
    def step(s):
        r, pc = s.r, s.pc
        w = s.m.r(pc, 4); op = w >> 26; d = (w >> 21) & 31; a = (w >> 16) & 31; b = (w >> 11) & 31
        i16 = sx(w & 0xFFFF, 16); k16 = w & 0xFFFF; npc = pc + 4
        def setr(x, v):
            if x: r[x] = v & M32
        if op == 0x00: npc = (pc + 4 * sx(w & 0x3FFFFFF, 26)) & M32
        elif op == 0x01: setr(9, pc + 4); npc = (pc + 4 * sx(w & 0x3FFFFFF, 26)) & M32
        elif op == 0x03: npc = (pc + 4 * sx(w & 0x3FFFFFF, 26)) & M32 if not s.f else npc
        elif op == 0x04: npc = (pc + 4 * sx(w & 0x3FFFFFF, 26)) & M32 if s.f else npc
        elif op == 0x05: pass
        elif op == 0x06: setr(d, k16 << 16)
        elif op == 0x11: npc = r[b]
        elif op == 0x12: t = r[b]; setr(9, pc + 4); npc = t
        elif op in (0x21, 0x22): setr(d, s.m.r((r[a] + i16) & M32, 4))
        elif op == 0x23: setr(d, s.m.r((r[a] + i16) & M32, 1))
        elif op == 0x24: setr(d, sx(s.m.r((r[a] + i16) & M32, 1), 8))
        elif op == 0x25: setr(d, s.m.r((r[a] + i16) & M32, 2))
        elif op == 0x26: setr(d, sx(s.m.r((r[a] + i16) & M32, 2), 16))
        elif op == 0x27: setr(d, r[a] + i16)
        elif op == 0x29: setr(d, r[a] & k16)
        elif op == 0x2A: setr(d, r[a] | k16)
        elif op == 0x2B: setr(d, r[a] ^ (i16 & M32))
        elif op == 0x2C: setr(d, sx(r[a], 32) * i16)
        elif op == 0x2D: setr(d, s.stubs.get(("mfspr", (r[a] | k16)), lambda c: 0)(s))
        elif op == 0x2E:
            n = w & 0x3F; t = (w >> 6) & 3
            setr(d, [r[a] << n, r[a] >> n, sx(r[a], 32) >> n, (r[a] >> n) | (r[a] << (32 - n))][t])
        elif op in (0x2F, 0x39):
            x, y = r[a], (i16 & M32 if op == 0x2F else r[b]); xs, ys = sx(x, 32), sx(y, 32)
            s.f = {0: x == y, 1: x != y, 2: x > y, 3: x >= y, 4: x < y, 5: x <= y, 0xA: xs > ys, 0xB: xs >= ys, 0xC: xs < ys, 0xD: xs <= ys}[d]
        elif op == 0x30: pass                                           # l.mtspr: ignored
        elif op in (0x35, 0x36, 0x37):
            imm = sx((((w >> 21) & 31) << 11) | (w & 0x7FF), 16)
            s.m.w((r[a] + imm) & M32, {0x35: 4, 0x36: 1, 0x37: 2}[op], r[b])
        elif op == 0x38:
            f = w & 0xF; t = (w >> 6) & 3; x, y = r[a], r[b]
            if f == 0: setr(d, x + y)
            elif f == 2: setr(d, x - y)
            elif f == 3: setr(d, x & y)
            elif f == 4: setr(d, x | y)
            elif f == 5: setr(d, x ^ y)
            elif f == 6: setr(d, sx(x, 32) * sx(y, 32))
            elif f == 8: n = y & 31; setr(d, [x << n, x >> n, sx(x, 32) >> n, (x >> n) | (x << (32 - n))][t])
            elif f == 9: setr(d, int(sx(x, 32) / sx(y, 32)) if y else 0)
            elif f == 0xA: setr(d, x // y if y else 0)
            elif f == 0xB: setr(d, x * y)
            else: raise RuntimeError(f"unhandled ALU op {w:08x} at {pc:#x}")
        else: raise RuntimeError(f"unhandled opcode {op:#x} ({w:08x}) at {pc:#010x}")
        s.pc = npc

def main():
    import os
    game = open(sys.argv[1], "rb").read(); nframes = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    hostdir = os.environ.get("EMU_DIR"); limit = int(os.environ.get("EMU_LIMIT", "50000000"))
    mem = Mem(); log = []; heap = [0x02150000]; t = [1000]; files = {}
    LOAD = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x02112C40
    SVC, FB = 0x021E0000, 0x02097340
    flashprog = os.environ.get("EMU_FLASH") == "1"
    if flashprog:
        mem.flash = bytearray(b"\xff" * 0x400000); mem.flash[0x2A0000:0x2A0000 + len(game)] = game
        h = struct.unpack_from("<14I", game)
        assert h[0] == 0x4D445848, "no HXDM header"
        for dst, src, n in (h[3:6], h[6:9]):
            mem.ram[dst - RAM0:dst - RAM0 + n] = mem.flash[src - 0x06000000:src - 0x06000000 + n]
        for dst, n in (h[9:11], h[11:13]): mem.ram[dst - RAM0:dst - RAM0 + n] = bytes(n)
        LOAD = h[2]                                                 # entry
    else:
        mem.ram[LOAD - RAM0:LOAD - RAM0 + len(game)] = game
    mem.w(0x02019FF8, 4, FB); mem.w(0x02019FF4, 2, 320); mem.w(0x02019FF6, 2, 240)
    HEAP_END = 0x021FE000 if os.environ.get("EMU_FLASH") != "1" else 0x021E2600   # flash program: ~870 KiB heap as in a game window
    def malloc(c):
        n = (c.r[3] + 63) & ~63; p = heap[0]
        if p + n > HEAP_END: log.append(f"malloc({c.r[3]}) -> 0 (full)"); return 0
        heap[0] += n; log.append(f"malloc({c.r[3]}) -> {p:#x}"); return p
    def now(c): t[0] += 7; return t[0]
    store, fds, fpos = {}, {}, {}                                # file name -> bytearray; fd -> name; fd -> position
    def fopen(c):
        n = bytearray(); a = c.r[3]
        while mem.r(a, 1): n.append(mem.r(a, 1)); a += 1
        n = n.decode(); mode = c.r[4]
        if n not in store and hostdir and os.path.exists(os.path.join(hostdir, n)): store[n] = bytearray(open(os.path.join(hostdir, n), "rb").read())
        if mode == 0xB: store[n] = bytearray()
        elif n not in store: log.append(f"f_open({n!r}, {mode:#x}) -> not found"); return 0xFFFFFFFF
        fd = next(i for i in range(4) if i not in fds); fds[fd] = n; fpos[fd] = 0
        if not n.endswith(".LOG"): log.append(f"f_open({n!r}, {mode:#x}) -> {fd}")
        return fd
    def fclose(c): fds.pop(c.r[3], None); return 0
    def fwrite(c):
        a, k, fd = c.r[4], c.r[5], c.r[3]; f = store[fds[fd]]; data = mem.ram[a - RAM0:a - RAM0 + k] if a >= RAM0 else mem.sram[a:a + k]
        p = fpos[fd]; f[p:p + k] = data; fpos[fd] = p + k
        if fds[fd].endswith(".LOG"): sys.stdout.write(bytes(data).decode(errors="replace")); sys.stdout.flush()
        return k
    def fread(c):
        fd, a, k = c.r[3], c.r[4], c.r[5]; f = store[fds[fd]]; p = fpos[fd]; data = f[p:p + k]
        mem.ram[a - RAM0:a - RAM0 + len(data)] = data; fpos[fd] = p + len(data); return len(data)
    def fseek(c): fpos[c.r[3]] = c.r[4]; return 0
    def largest(c): return HEAP_END - heap[0]
    stubs = {0x060691EC: lambda c: log.append(f"dcache_INVALIDATE({c.r[3]:#x}, {c.r[4]})"),
             0x02004524: lambda c: log.append(f"dcache_writeback({c.r[3]:#x}, {c.r[4]})"),
             0x060691A8: lambda c: log.append("icache_reset()"),
             0x00001034: lambda c: None, 0x06039198: lambda c: log.append("present()"),
             0x0602F6AC: now, 0x0602DC20: malloc, 0x0602DE64: lambda c: log.append(f"free({c.r[3]:#x})"),
             0x0607BC9C: lambda c: (mem.ram.__setitem__(slice(c.r[3] - RAM0, c.r[3] - RAM0 + c.r[5]), mem.ram[c.r[4] - RAM0:c.r[4] - RAM0 + c.r[5]]), c.r[3])[1],
             0x060660DC: lambda c: log.append("set_palette()"), 0x0601CD60: fopen, 0x0601CF28: fwrite,
             0x0601CE38: fclose, 0x0601CEB0: fread, 0x0601D3C8: lambda c: len(store[fds[c.r[3]]]), 0x0601D018: fseek, 0x0602E040: largest,
             # audio (docs/analysis/sound.md): start returns 1, the rest do nothing (no interrupts are emulated)
             0x06027804: lambda c: (log.append(f"audio_start({c.r[3]}, {c.r[4]:#x}, {c.r[5]})"), 1)[1], 0x06027860: lambda c: None,
             0x06027888: lambda c: None, 0x060278E8: lambda c: None, 0x02002B38: lambda c: None}
    svc = [0x48585356, 2, 0x06039198, 0x0602F6AC, 0x00001034, 0x0607BC9C, 0x0602DC20, 0x0602DE64, 0x02019FF8, 0x02019FF4, 0x02019FF6,
           0x060660DC, 0x0601CD60, 0x0601CEB0, 0x0601CF28, 0x0601CE38, 0x0601D3C8]
    for i, v in enumerate(svc): mem.w(SVC + 4 * i, 4, v)
    cpu = CPU(mem, stubs); cpu.r[1] = 0x021F0000
    heap[0] = 0x02109600 if flashprog else (LOAD + len(game) + 63) & ~63   # flash program: the camera's game-window heap start
    pfile = os.environ.get("EMU_PROFILE")
    import array
    words = len(game) // 4 + 1
    p_init, p_frames = (array.array("I", bytes(4 * words)), array.array("I", bytes(4 * words))) if pfile else (None, None)
    def save_profile():
        if pfile:
            with open(pfile, "wb") as f: f.write(struct.pack("<4sII", b"EMUP", LOAD, words)); f.write(p_init.tobytes()); f.write(p_frames.tobytes())
    if pfile and flashprog: pfile = None                             # (profiles are for relocatable builds)
    if pfile: cpu.prof = (p_init, LOAD, words)
    ctx = cpu.call(LOAD, [SVC, 0, 0, 0], limit=limit); print(f"INIT -> ctx {ctx:#x}  ({cpu.steps} instructions)")
    if pfile: cpu.prof = (p_frames, LOAD, words); save_profile()
    for f in range(nframes):
        if not ctx: break
        ctx = cpu.call(LOAD, [SVC, 1, 0, ctx], limit=limit); print(f"FRAME {f} -> ctx {ctx:#x}  ({cpu.steps} instructions)")
        if pfile and f % 25 == 24: save_profile()
    save_profile(); cpu.prof = None
    cpu.call(LOAD, [SVC, 2, 0, ctx], limit=limit); print("QUIT done")
    for l in log[:40]: print("  ", l)
    for n, data in store.items():
        if len(data) <= 4096: print(f"  file {n}: {bytes(data)!r}")
    print("first framebuffer bytes:", mem.ram[FB - RAM0:FB - RAM0 + 16].hex())
    if os.environ.get("EMU_SHOT"):                                  # the 320x240 8-bit layer as a PGM (palette indices as grey levels)
        with open(os.environ["EMU_SHOT"], "wb") as f: f.write(b"P5 320 240 255\n" + bytes(mem.ram[FB - RAM0:FB - RAM0 + 320 * 240]))
main()
