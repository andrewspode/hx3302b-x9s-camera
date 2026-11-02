/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN: BARS vertical bars (value 0xFF) on a value-0 background for 3 s, so the user can tell which file a button loaded. */
#include "services.h"
void __attribute__((section(".text.entry"))) entry(const struct hx_services *s)
{
    if (s->magic != HX_SERVICES_MAGIC) return;
    u8 *fb = *s->display_buffer; u32 w = *s->width, h = *s->height;
    if (!fb || !w || !h || w > 2048 || h > 2048) return;
    u32 slot = w / (2 * BARS + 1);
    for (u32 y = 0; y < h; y++) { for (u32 x = 0; x < w; x++) fb[y * w + x] = ((x / slot) & 1) && x / slot < 2 * BARS ? 0xFF : 0x00; s->wdt_kick(); }
    s->present();
    u32 t0 = s->now_ms();
    for (u32 i = 0; i < 20000000 && s->now_ms() - t0 < 3000; i++) s->wdt_kick();
}
