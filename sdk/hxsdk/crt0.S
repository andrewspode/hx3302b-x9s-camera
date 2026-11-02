/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN start: the game host calls byte 0 as entry(svc, cmd, keys, ctx). A branch over the header (filled in by mkgame.py), then
   find our own load address WITHOUT any relocated value (l.jal puts the address of the next instruction in r9; no delay slot) and pass
   it to the C start code as a 5th argument (r7).
   Header (offsets from the image start): 4 'RELO', 8 table offset, 12 count, 16 done, 20 image size, 24 bss start, 28 bss size. */
    .section .text.entry,"ax"
    .global _start
_start:
    l.j     2f
    .global __hx_header
__hx_header:
    .word   0x4F4C4552
    .word   0, 0, 0, 0, 0, 0
2:  l.or    r12, r9, r0         /* keep the caller's return address */
    l.jal   1f                  /* r9 = address of 1: */
1:  l.addi  r7, r9, -(1b - _start)
    l.or    r9, r12, r0
    l.j     __hx_crt0
