/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN test: 256 horizontal bands, band n = pixel value n, shown for 5 s, then return to the loader.
   Loaded at an unknown RAM address, so it must be position independent by construction: no globals, no strings, no jump tables
   (build.sh checks the object has no relocations). Everything external comes through the services table. */
#include "services.h"

void __attribute__((section(".text.entry"))) entry(const struct hx_services *s)
{
    if (s->magic != HX_SERVICES_MAGIC) return;
    u8 *fb = *s->display_buffer;
    u32 w = *s->width, h = *s->height;
    if (!fb || !w || !h || w > 2048 || h > 2048) return;
    for (u32 y = 0; y < h; y++) {
        u8 v = (u8)((y * 256) / h);
        for (u32 x = 0; x < w; x++) fb[y * w + x] = v;
        s->wdt_kick();
    }
    s->present();
    u32 t0 = s->now_ms();
    for (u32 i = 0; i < 20000000 && s->now_ms() - t0 < 5000; i++) s->wdt_kick();   /* bounded even if the clock does not tick */
}
