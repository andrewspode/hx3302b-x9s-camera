#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
set -eu
R=$(cd "$(dirname "$0")/.." && pwd); OR1KLE=${OR1KLE:-$R/out/toolchain}; T=$OR1KLE/bin; O=$R/out/usb; mkdir -p $O
[ -x $T/or1kle-elf-gcc ] || sh $R/toolchain/setup.sh $OR1KLE
CF="-O2 -fno-pic -mhard-div -mhard-mul -ffreestanding -nostdlib -fno-jump-tables -Wall -Wextra -Werror"
$T/or1kle-elf-gcc $CF -c $R/tools/usb_read_stub.c -o $O/usb_read_stub.o
$T/or1kle-elf-ld -o $O/usb_read_stub.elf -Ttext=0x81f0 -e entry --no-warn-rwx-segments \
  --defsym fw_memcpy=0x0607BC9C --defsym fw_send=0x06024844 \
  -Map $O/usb_read_stub.map $O/usb_read_stub.o
$T/or1kle-elf-objcopy -O binary -j .text $O/usb_read_stub.elf $O/usb_read_stub.bin
[ "$($T/or1kle-elf-nm $O/usb_read_stub.elf | awk '$3=="entry"{print $1}')" = "000081f0" ] || { echo "entry is not at 0x81f0" >&2; exit 1; }
[ -z "$($T/or1kle-elf-readelf -S $O/usb_read_stub.elf | grep -E ' \.(rodata|data|bss|sdata|sbss) ' | awk '$6 != "00000000"')" ] || { echo "stub has data sections" >&2; exit 1; }
SIZE=$(stat -c %s $O/usb_read_stub.bin)
[ $((0x81f0 + SIZE)) -le $((0x8244)) ] || { echo "stub overruns the USB response buffer" >&2; exit 1; }
echo "usb_read_stub.bin $SIZE bytes at 0x81f0"
