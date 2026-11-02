/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN: display information dump, draws nothing. Writes DISP.BIN:
   0x000  u32 magic 0x50534944 ('DISP'), u32 version 1
   0x008  u32 words: [0]=*(u16*)0x0201A1A4 (W) [1]=*(u16*)0x0201A1A6 (H) [2]=*(u32*)0x02019FF0 (layer object?) [3]=*(u32*)0x02019FF8 (buffer)
   0x018  0x100 bytes from the layer object *(u32*)0x02019FF0 (zeros if it is not a RAM pointer)
   0x118  0x400 bytes of RAM 0x02019E00-0x0201A1FF (the display globals and W/H)
   0x518  0x100 bytes from the buffer's start (to see what the firmware left there) */
#include "fwfile.h"
static void copy(u8 *d, const u8 *s, u32 n) { for (u32 i = 0; i < n; i++) d[i] = s[i]; }
static int ram(u32 a) { return a >= 0x02000000 && a < 0x021F0000; }
void __attribute__((section(".text.entry"))) entry(const struct hx_services *s)
{
    if (s->magic != HX_SERVICES_MAGIC) return;
    u32 len = 0x618;
    u8 *b = (u8 *)s->malloc(len);
    if (!b) return;
    for (u32 i = 0; i < len; i++) b[i] = 0;
    u32 *w = (u32 *)b;
    u32 layer = *(volatile u32 *)0x02019FF0, buf = *(volatile u32 *)0x02019FF8;
    w[0] = 0x50534944; w[1] = 1;
    w[2] = *(volatile u16 *)0x0201A1A4; w[3] = *(volatile u16 *)0x0201A1A6; w[4] = layer; w[5] = buf;
    if (ram(layer)) copy(b + 0x18, (const u8 *)layer, 0x100);
    copy(b + 0x118, (const u8 *)0x02019E00, 0x400);
    if (ram(buf)) copy(b + 0x518, (const u8 *)buf, 0x100);
    char name[9];
    name[0] = 'D'; name[1] = 'I'; name[2] = 'S'; name[3] = 'P'; name[4] = '.'; name[5] = 'B'; name[6] = 'I'; name[7] = 'N'; name[8] = 0;
    hx_write_file(name, b, len);
    s->free(b);
}
