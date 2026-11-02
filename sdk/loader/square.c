/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN for the game host: a 20x20 yellow square on black, moved by the d-pad at 120 px/s; a bar along the top is the
   frame rate (4 px per fps, white). OK (shutter) held: the square turns white. On quit writes FPS.TXT: frames, ms, min/max frame ms. */
#include "services.h"
struct ctx { u32 x, y, t0, last, frames, minf, maxf, fps, fps_t, fps_n; };
static void put(char *o, u32 *n, u32 v) { char t[12]; u32 k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) o[(*n)++] = t[--k]; o[(*n)++] = '\n'; }

void *__attribute__((section(".text.entry"))) entry(const struct hx_services *s, u32 cmd, u32 keys, void *vctx)
{
    struct ctx *c = (struct ctx *)vctx;
    if (s->magic != HX_SERVICES_MAGIC || s->version < 2) return 0;
    if (cmd == HX_INIT) {
        c = (struct ctx *)s->malloc(sizeof *c);
        if (!c) return 0;
        c->x = 150 << 8; c->y = 110 << 8; c->t0 = c->last = c->fps_t = s->now_ms(); c->frames = 0; c->minf = 0xFFFFFFFF; c->maxf = 0; c->fps = 0; c->fps_n = 0;
        return c;
    }
    if (cmd == HX_QUIT) {
        if (!c) return 0;
        char out[64]; u32 n = 0;
        put(out, &n, c->frames); put(out, &n, s->now_ms() - c->t0); put(out, &n, c->minf); put(out, &n, c->maxf);
        char name[8]; name[0] = 'F'; name[1] = 'P'; name[2] = 'S'; name[3] = '.'; name[4] = 'T'; name[5] = 'X'; name[6] = 'T'; name[7] = 0;
        int fd = s->f_open(name, 0xB);
        if (fd >= 0) { s->f_write(fd, out, n); s->f_close(fd); }
        s->free(c);
        return 0;
    }
    u8 *fb = *s->display_buffer;
    u32 w = *s->width, h = *s->height;
    if (!fb || w != 320 || h != 240) return c;
    u32 now = s->now_ms(), dt = now - c->last;
    c->last = now;
    if (c->frames) { if (dt < c->minf) c->minf = dt; if (dt > c->maxf) c->maxf = dt; }
    c->frames++; c->fps_n++;
    if (now - c->fps_t >= 1000) { c->fps = c->fps_n * 1000 / (now - c->fps_t); c->fps_t = now; c->fps_n = 0; }
    if (dt > 100) dt = 100;
    u32 step = dt * 120 * 256 / 1000;                              /* 120 px/s in 8.8 fixed point */
    if ((keys & HX_KEY_LEFT)  && c->x >= step) c->x -= step;
    if ((keys & HX_KEY_RIGHT) && c->x + step <= (300u << 8)) c->x += step;
    if ((keys & HX_KEY_UP)    && c->y >= step + (8u << 8)) c->y -= step;
    if ((keys & HX_KEY_DOWN)  && c->y + step <= (220u << 8)) c->y += step;
    u32 *p = (u32 *)fb;
    for (u32 i = 0; i < w * h / 4; i++) p[i] = 0x07070707;          /* 7 = black (stock palette) */
    u32 bar = c->fps * 4; if (bar > w) bar = w;
    for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < bar; x++) fb[y * w + x] = 2;   /* 2 = white */
    u8 col = (keys & HX_KEY_OK) ? 2 : 1;                           /* 1 = yellow */
    u32 sx = c->x >> 8, sy = c->y >> 8;
    for (u32 y = sy; y < sy + 20; y++) for (u32 x = sx; x < sx + 20; x++) fb[y * w + x] = col;
    s->present();
    return c;
}
