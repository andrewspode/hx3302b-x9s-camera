#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Andrew Spode
# Turn stock binutils 2.43.1 + GCC 14.2.0 (OpenRISC: big-endian, delay slots) into a PRIVATE toolchain for the HX330x CPU:
# little-endian and no branch delay slots, everywhere, no options. Installed with the program prefix or1kle-elf- so it cannot be
# mistaken for a standard OpenRISC toolchain. Usage: patch-sources.sh SRC_DIR (holding binutils-2.43.1/ and gcc-14.2.0/).
# Each edit checks that the original text is present first, so a wrong source version fails loudly.
set -eu
S=$1
edit() { # file, old, new (fixed strings, first match must exist)
  grep -qF -- "$2" "$1" || { echo "patch: expected text not found in $1: $2" >&2; exit 1; }
  python3 -I -c 'import sys; p,o,n=sys.argv[1:4]; s=open(p).read(); open(p,"w").write(s.replace(o,n,1))' "$1" "$2" "$3"
}
B=$S/binutils-2.43.1
# BFD: the only or1k object vector becomes little-endian (same vector symbol and name, so configure/targets need no changes)
edit $B/bfd/elf32-or1k.c '#define TARGET_BIG_SYM			or1k_elf32_vec' '#define TARGET_LITTLE_SYM		or1k_elf32_vec'
edit $B/bfd/elf32-or1k.c '#define TARGET_BIG_NAME			"elf32-or1k"' '#define TARGET_LITTLE_NAME		"elf32-or1k"'
# GAS: little-endian output and little-endian CGEN instruction encoding
edit $B/gas/config/tc-or1k.h '#define TARGET_BYTES_BIG_ENDIAN 1' '#define TARGET_BYTES_BIG_ENDIAN 0'
edit $B/gas/config/tc-or1k.c 'CGEN_ENDIAN_BIG,' 'CGEN_ENDIAN_LITTLE,'
edit $B/gas/config/tc-or1k.c 'number_to_chars_bigendian (buf, val, n);' 'number_to_chars_littleendian (buf, val, n);'

G=$S/gcc-14.2.0/gcc/config/or1k
# GCC: little-endian data layout
edit $G/or1k.h '#define BYTES_BIG_ENDIAN 1' '#define BYTES_BIG_ENDIAN 0'
edit $G/or1k.h '#define WORDS_BIG_ENDIAN 1' '#define WORDS_BIG_ENDIAN 0'
# GCC: no delay slots. Without define_delay nothing is scheduled into a slot; '%#' must then not add the filler l.nop either.
edit $G/or1k.md '(define_delay (eq_attr "type" "control")
  [(eq_attr "type" "alu,st,ld") (nil) (nil)])' '; HX330x: this CPU has no branch delay slots (define_delay removed).'
edit $G/or1k.cc '	fputs ("\n\t l.nop\n", file);' '	; /* HX330x: no delay slots, nothing to fill.  */'
# PIC is NOT supported: set_got uses the "l.jal 8" delay-slot trick. Always compile with -fno-pic.
echo "sources patched"
