/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN image, linked at 0 and relocated at load time by crt0. One contiguous image: entry stub + header, code, read-only data,
   data, bss (zero-filled by crt0). Keep --emit-relocs (-q) so mkgame.py can build the relocation table. */
ENTRY(_start)
SECTIONS {
  . = 0;                  /* build.sh's self-check links a copy with this line changed to another base */
  .text : { KEEP(*(.text.entry)) *(.text .text.*) }
  .rodata : ALIGN(4) { *(.rodata .rodata.*) }
  .data : ALIGN(4) { *(.data .data.* .sdata .sdata.*) }
  .bss : ALIGN(4) { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); __bss_end = .; }
  /DISCARD/ : { *(.comment) *(.eh_frame) }
}
