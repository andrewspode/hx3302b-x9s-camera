/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN: palette chart with the real layer geometry (layer at *0x02019FF0: width/height u16 at 0x02019FF4/6, buffer *0x02019FF8,
   stride = width). 16 x 16 grid, cell (col c, row r) = pixel value 16*r + c, so value 0 is top-left, 0xFF bottom-right; a 1-pixel value-0xFF
   border around the whole layer marks its edges. Then the palette test (palette entry = alpha5 << 16 | RGB565; alpha 0x1F opaque, 0 clear):
   6 s stock palette -> 6 s custom RGB332-style palette (all opaque) set with fn_060660DC(0, pal) -> stock palette restored by reading
   resource 64 (fn_0603817C(0x40, buf), as the firmware does at 0x060419B0) and setting it again. */
#include "services.h"
typedef void (*set_pal_t)(u32 layer, const u32 *pal);
typedef int  (*res_read_t)(u32 id, void *buf);
#define SET_PALETTE ((set_pal_t)0x060660DC)
#define RES_READ    ((res_read_t)0x0603817C)
static void wait_ms(const struct hx_services *s, u32 ms)
{
    u32 t0 = s->now_ms();
    for (u32 i = 0; i < 40000000 && s->now_ms() - t0 < ms; i++) s->wdt_kick();
}
void __attribute__((section(".text.entry"))) entry(const struct hx_services *s)
{
    if (s->magic != HX_SERVICES_MAGIC) return;
    u8 *fb = *(u8 *volatile *)0x02019FF8;
    u32 w = *(volatile u16 *)0x02019FF4, h = *(volatile u16 *)0x02019FF6;
    if (!fb || w != 320 || h != 240) return;                 /* only draw if the geometry is what DISP.BIN showed */
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u32 v = 16 * (y * 16 / h) + (x * 16 / w);
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) v = 0xFF;
            fb[y * w + x] = (u8)v;
        }
        s->wdt_kick();
    }
    s->present();
    wait_ms(s, 6000);
    u32 *pal = (u32 *)s->malloc(0x400);
    if (!pal) return;
    for (u32 i = 0; i < 256; i++) {
        u32 r = i & 0xE0, g = (i << 3) & 0xE0, b = (i << 6) & 0xC0;
        pal[i] = (0x1Fu << 16) | ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    SET_PALETTE(0, pal);
    s->present();
    wait_ms(s, 6000);
    if (RES_READ(0x40, pal) > 0) SET_PALETTE(0, pal);                 /* back to the stock palette */
    s->present();
    s->free(pal);
}
