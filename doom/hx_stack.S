/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 Andrew Spode */
/* void *hx_on_stack(svc, cmd, keys, ctx, stack_top, fn): call fn(svc, cmd, keys, ctx) with r1 on our own stack, then return on the caller's.
   Doom runs inside a firmware event handler whose stack is in the small SRAM (parameter pointers seen at ~0x6FA8); rendering a level
   recurses through the BSP tree and needs far more. No delay slots on this CPU. */
    .text
    .global hx_on_stack
hx_on_stack:
    l.addi  r7, r7, -16
    l.sw    0(r7), r1           /* caller's stack pointer */
    l.sw    4(r7), r9           /* caller's return address */
    l.or    r1, r7, r0
    l.jalr  r8
    l.lwz   r9, 4(r1)
    l.lwz   r1, 0(r1)
    l.jr    r9
