#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
# Build a relocatable GAME.BIN with the hxsdk: build.sh NAME (NAME.c [+ more .c files]) -> out/hxsdk/NAME.GAME.BIN
# Self-check: links the same objects at base 0x02345640 too and verifies that mkgame's relocation, applied in Python, gives the same bytes.
set -eu
EXTRA_CFLAGS=${EXTRA_CFLAGS:-}
R=$(cd "$(dirname "$0")/../.." && pwd); OR1KLE=${OR1KLE:-$R/out/toolchain}; T=$OR1KLE/bin; O=$R/out/hxsdk; S=$R/sdk/hxsdk; mkdir -p $O
[ -x $T/or1kle-elf-gcc ] || sh $R/toolchain/setup.sh $OR1KLE
N=$1; shift
LDW="--no-warn-rwx-segments"
CF="-O2 -fno-pic -mhard-div -mhard-mul -ffreestanding -nostdlib -Wall -Wextra -I$S"
$T/or1kle-elf-gcc $CF -c $S/crt0.S -o $O/crt0.o
$T/or1kle-elf-gcc $CF ${HX_DEBUG:+-DHX_DEBUG} -fno-tree-loop-distribute-patterns -fno-jump-tables -c $S/crt0.c -o $O/crt0c.o
OBJS="$O/crt0.o $O/crt0c.o"
SRCN=${SRC:-$N}
for f in $S/$SRCN.c "$@"; do o=$O/$N.$(basename ${f%.c}).o; $T/or1kle-elf-gcc $CF ${HX_DEBUG:+-DHX_DEBUG} $EXTRA_CFLAGS -c $f -o $o; OBJS="$OBJS $o"; done
FW="--defsym fw_wdt_kick=0x00001034"
$T/or1kle-elf-ld $LDW -q -T $S/hx.ld $FW -o $O/$N.elf $OBJS $($T/or1kle-elf-gcc -print-libgcc-file-name)
sed 's/^  \. = 0;/  . = 0x02345640;/' $S/hx.ld > $O/hx_base.ld
$T/or1kle-elf-ld $LDW -q -T $O/hx_base.ld $FW -o $O/$N.base.elf $OBJS $($T/or1kle-elf-gcc -print-libgcc-file-name)
python3 -I $S/mkgame.py $O/$N.elf $O/$N.GAME.BIN
python3 -I $S/relocheck.py $O/$N.GAME.BIN $O/$N.base.elf 0x02345640
sed 's/^  \. = 0;/  . = 0x0234C9C0;/' $S/hx.ld > $O/hx_base2.ld
$T/or1kle-elf-ld $LDW -q -T $O/hx_base2.ld $FW -o $O/$N.base2.elf $OBJS $($T/or1kle-elf-gcc -print-libgcc-file-name)
python3 -I $S/relocheck.py $O/$N.GAME.BIN $O/$N.base2.elf 0x0234C9C0
