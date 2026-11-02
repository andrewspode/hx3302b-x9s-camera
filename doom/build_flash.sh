#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Andrew Spode
# Path B build: Doom split between flash (cold code, in place at 0x062A0000) and fixed RAM (0x02023000 + 0x020F5000), no relocation.
# Runs build.sh first (same objects), recompiles the platform files with -DHX_FLASH, splits with mksplit.py from an emulator profile
# (PROFILE, default doom/profile/profile.bin with its map profile.map), links with the generated script.
# Out: out/doom/doom_flash.elf, out/doom/DOOM.FLASH.BIN (the flash image from 0x062A0000: header, cold code, then the RAM part's load copy).
set -eu
R=$(cd "$(dirname "$0")/.." && pwd); OR1KLE=${OR1KLE:-$R/out/toolchain}; T=$OR1KLE/bin; S=$R/sdk/hxsdk; D=$R/doom; O=$R/out/doom
[ -x $T/or1kle-elf-gcc ] || sh $R/toolchain/setup.sh $OR1KLE
PROFILE=${PROFILE:-$D/profile/profile.bin}; PMAP=${PMAP:-$D/profile/profile.map}
OPT=${OPT:--Os} $D/build.sh > /dev/null
CF="-ffile-prefix-map=$R=. ${OPT:--Os} ${DOOM_DEFS:-} -DHX_FLASH -fno-pic -mhard-div -mhard-mul -ffunction-sections -fdata-sections -fno-strict-aliasing -DCMAP256 -DFEATURE_SOUND -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX -D_DEFAULT_SOURCE -I$S -I$O/src"
mkdir -p $O/fobj
OBJS=""
for o in $O/obj/*.o; do
  b=$(basename $o .o)
  case $b in crt0|crt0c) continue ;; hx_doom|hx_syscalls|hx_sound|hx_music) $T/or1kle-elf-gcc $CF -Wall -c $D/$b.c -o $O/fobj/$b.o; OBJS="$OBJS $O/fobj/$b.o" ;; *) OBJS="$OBJS $o" ;; esac
done
python3 -I $D/mksplit.py $PMAP $PROFILE $O/doom_flash.ld
LIBS="$OR1KLE/or1k-elf/lib/libc.a $OR1KLE/or1k-elf/lib/libm.a $($T/or1kle-elf-gcc -print-libgcc-file-name)"
$T/or1kle-elf-ld --no-warn-rwx-segments --gc-sections -e hx_main -u hx_fprog -T $O/doom_flash.ld -Map $O/doom_flash.map -o $O/doom_flash.elf $OBJS --start-group $LIBS --end-group
$T/or1kle-elf-objcopy -O binary -j .fhdr -j .ftext -j .text -j .data $O/doom_flash.elf $O/DOOM.FLASH.BIN
$T/or1kle-elf-readelf -SW $O/doom_flash.elf | awk '$2 ~ /^\.(fhdr|ftext|text|data|bss2|bss)$/ {print "  " $2, "addr 0x" $4, "size 0x" $6}'
python3 -I - $O/DOOM.FLASH.BIN <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
h = struct.unpack_from("<14I", d)
assert h[0] == 0x4D445848 and h[1] == 1, "no HXDM header at the start"
(td, ts, tl), (dd, ds, dl) = h[3:6], h[6:9]; (b1, b1l), (b2, b2l) = h[9:11], h[11:13]; fend = h[13]
print(f"DOOM.FLASH.BIN {len(d)} bytes (flash 0x062A0000-{0x062A0000 + len(d):#x}); entry {h[2]:#x}")
print(f"  copy text {ts:#x} -> {td:#x} +{tl:#x}; data {ds:#x} -> {dd:#x} +{dl:#x}; zero {b1:#x} +{b1l:#x}, {b2:#x} +{b2l:#x}")
assert 0x062A0000 + len(d) == fend <= 0x063F0000, "flash part too big or misplaced"
assert ts + tl <= fend and ds + dl <= fend and 0x02023000 <= td and dd + dl <= b1 and b1 + b1l <= 0x02097000 and 0x020F5000 <= b2 and b2 + b2l <= 0x02109000
print(f"  RAM area 1 used to {b1 + b1l:#x} (free to 0x2097000: {0x02097000 - (b1 + b1l)} bytes); area 2 used to {b2 + b2l:#x} (free {0x02109000 - (b2 + b2l)})")
PY
