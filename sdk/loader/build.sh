#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
# Build the loader (for the fly slot) and the GAME.BIN test into out/loader/. Checks: loader fits before Maze's code (0x0604D94C);
# GAME.BIN has no relocations (position independent) and starts with entry().
set -eu
R=$(cd "$(dirname "$0")/../.." && pwd); OR1KLE=${OR1KLE:-$R/out/toolchain}; T=$OR1KLE/bin; O=$R/out/loader; mkdir -p $O
[ -x $T/or1kle-elf-gcc ] || sh $R/toolchain/setup.sh $OR1KLE
CF="-O2 -fno-pic -mhard-div -mhard-mul -ffreestanding -nostdlib -fno-jump-tables -Wall -Wextra -Werror"
$T/or1kle-elf-gcc $CF -c $R/sdk/loader/${LOADER:-host}.c -o $O/loader.o
$T/or1kle-elf-ld -o $O/loader.elf -Ttext=0x0604CBA0 -e hx_ok --no-warn-rwx-segments \
  --defsym fw_f_open=0x0601CD60 --defsym fw_f_size=0x0601D3C8 --defsym fw_f_read=0x0601CEB0 --defsym fw_f_close=0x0601CE38 --defsym fw_f_write=0x0601CF28 \
  --defsym fw_malloc=0x0602DC20 --defsym fw_free=0x0602DE64 --defsym fw_memcpy=0x0607BC9C \
  --defsym fw_dcache_inval=0x060691EC --defsym fw_dcache_wb=0x02004524 --defsym fw_icache_reset=0x060691A8 \
  --defsym fw_present=0x06039198 --defsym fw_set_palette=0x060660DC --defsym fw_post=0x0602F36C --defsym fw_mode_request=0x060472E8 --defsym fw_palette_restore=0x0604196C --defsym fw_dev=0x0601450C --defsym fw_now_ms=0x0602F6AC --defsym fw_wdt_kick=0x00001034 \
  -Map $O/loader.map $O/loader.o
[ -z "$($T/or1kle-elf-readelf -S $O/loader.elf | grep -E ' \.(data|bss|sdata|sbss) ' | awk '$6 != "00000000"')" ] || { echo "loader has writable data" >&2; exit 1; }
$T/or1kle-elf-objcopy -O binary $O/loader.elf $O/loader.bin
$T/or1kle-elf-nm $O/loader.elf | grep -E ' (hx_ok|hx_stub|hx_k1[5-8]|hx_open|hx_power|hx_back|hx_close|hx_cclose|hx_frame)$' > $O/loader.syms
END=$((0x0604CBA0 + $(stat -c %s $O/loader.bin)))
[ $END -le $((0x0604D94C)) ] || { echo "loader too big: ends $END" >&2; exit 1; }
# (the one-shot programs bench/bars/dispinfo/palette/memsurvey/game_test use the old calling convention; build them with LOADER=loader)
$T/or1kle-elf-gcc $CF -c $R/sdk/loader/game_test.c -o $O/game_test.o
# only PC-relative call relocations (R_OR1K_INSN_REL_26) are position independent; anything else means an absolute address
pic_check() { $T/or1kle-elf-readelf -rW $1 | awk '/R_OR1K/ && $3 != "R_OR1K_INSN_REL_26"' | grep . && { echo "$1 has absolute relocations" >&2; exit 1; } || true; }
pic_check $O/game_test.o
$T/or1kle-elf-ld -o $O/game_test.elf -T $R/sdk/loader/game.ld -e entry $O/game_test.o
$T/or1kle-elf-objcopy -O binary -j .text $O/game_test.elf $O/GAME.BIN
[ "$($T/or1kle-elf-nm $O/game_test.elf | awk '$3=="entry"{print $1}')" = "00000000" ] || { echo "entry is not at offset 0" >&2; exit 1; }
for P in square; do
  case $P in bars*) $T/or1kle-elf-gcc $CF -DBARS=${P#bars} -c $R/sdk/loader/bars.c -o $O/$P.o ;; *) $T/or1kle-elf-gcc $CF -c $R/sdk/loader/$P.c -o $O/$P.o ;; esac
  pic_check $O/$P.o
  $T/or1kle-elf-ld -o $O/$P.elf -T $R/sdk/loader/game.ld -e entry $O/$P.o
  $T/or1kle-elf-objcopy -O binary -j .text $O/$P.elf $O/$P.GAME.BIN
  [ "$($T/or1kle-elf-nm $O/$P.elf | awk '$3=="entry"{print $1}')" = "00000000" ] || { echo "$P: entry is not at offset 0" >&2; exit 1; }
  [ -z "$($T/or1kle-elf-readelf -S $O/$P.elf | grep -E ' \.(rodata|data|bss)' | awk '$6 != "00000000"')" ] || { echo "$P has data sections" >&2; exit 1; }
done
echo "loader.bin $(stat -c %s $O/loader.bin) bytes (ends $(printf %#x $END)); GAME.BIN $(stat -c %s $O/GAME.BIN) bytes"; cat $O/loader.syms
