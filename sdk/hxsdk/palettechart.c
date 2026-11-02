/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN (hxsdk, game host): the display layer's stock palette as a 16 x 16 grid of its 256 entries, cell (column c, row r) = index 16*r + c,
   with thin black lines between the cells. Press any key to stop. A small example of a program for the game host: draw into the layer, present
   every frame, quit on a key. */
#include "services.h"

#define COLS 16
#define ROWS 16
#define LINE 7                                  /* stock palette entry 7 = black, opaque */

static void draw(const struct hx_services *s)
{
    u8 *fb = *s->display_buffer;
    u32 w = *s->width, h = *s->height;           /* the layer: 320 x 240 */
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u32 c = x * COLS / w, r = y * ROWS / h;
            int edge = (x * COLS / w != (x + 1) * COLS / w) || (y * ROWS / h != (y + 1) * ROWS / h) || x == 0 || y == 0 || x == w - 1 || y == h - 1;
            fb[y * w + x] = edge ? (u8)LINE : (u8)(ROWS * r + c);
        }
        s->wdt_kick();
    }
}

void *hx_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx)
{
    (void)ctx;
    switch (cmd) {
    case HX_INIT:
        draw(s);
        return (void *)s;                        /* any non-zero value = run */
    case HX_FRAME:
        s->present();
        return keys ? 0 : ctx;                   /* stop on any key */
    default:                                     /* HX_QUIT: nothing allocated */
        return 0;
    }
}
