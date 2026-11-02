/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN (hxsdk, game host): finds the sizes of the CPU's data and instruction caches by timing. Writes CACHE.TXT to the card, one result per line:
     alu <ms>                  16 M iterations of a 4-instruction loop (64 M instructions), the speed reference
     dread <KiB> <ms>          read a working set of that size repeatedly, 8 MiB in total
     dwrite <KiB> <ms>         the same for writes
     icode <KiB> <ms>          run straight-line code of that size repeatedly, 32 M instructions in total
   The speed drops when the working set no longer fits the cache. The instruction sweep writes its own code (l.addi r3,r3,1 ... l.jr r9) into
   a buffer, so no assembler or linker tricks are needed. Build with QUICK=1 (-DQUICK) for a short run in the emulator. */
#include "services.h"

#ifdef QUICK
#define DIV 64
#else
#define DIV 1
#endif
#define dcache_writeback ((void (*)(void *, u32))0x02004524)   /* firmware: push dirty data cache lines to RAM */
#define icache_reset     ((void (*)(void))0x060691A8)          /* firmware: disable, reset and re-enable the instruction cache */

static u32 alu_loop(u32 n) { u32 x = 0; __asm__ volatile ("1: l.addi %0,%0,-1\n l.xor %1,%1,%0\n l.sfne %0,r0\n l.bf 1b" : "+r"(n), "+r"(x)); return x; }

static u32 read_ws(const volatile u32 *p, u32 ws_bytes, u32 total_bytes, const struct hx_services *s)
{
    u32 sum = 0, words = ws_bytes / 4, passes = total_bytes / ws_bytes;
    for (u32 k = 0; k < passes; k++) {
        const volatile u32 *q = p;
        for (u32 i = 0; i < words; i += 8)
            sum += q[i] + q[i+1] + q[i+2] + q[i+3] + q[i+4] + q[i+5] + q[i+6] + q[i+7];
        if ((k & 255) == 0) s->wdt_kick();
    }
    return sum;
}
static void write_ws(volatile u32 *p, u32 ws_bytes, u32 total_bytes, const struct hx_services *s)
{
    u32 words = ws_bytes / 4, passes = total_bytes / ws_bytes;
    for (u32 k = 0; k < passes; k++) {
        volatile u32 *q = p;
        for (u32 i = 0; i < words; i += 8) { q[i] = k; q[i+1] = k; q[i+2] = k; q[i+3] = k; q[i+4] = k; q[i+5] = k; q[i+6] = k; q[i+7] = k; }
        if ((k & 255) == 0) s->wdt_kick();
    }
}

/* the result file, kept in a static buffer and rewritten after every line so a crash still leaves the earlier lines */
static char out[2048];
static u32 outn;
static void put_str(const char *t) { while (*t) out[outn++] = *t++; }
static void put_num(u32 v)
{
    char t[12]; u32 n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[outn++] = t[--n];
}
static void line(const struct hx_services *s, const char *kind, u32 size, u32 ms)
{
    put_str(kind); out[outn++] = ' ';
    if (size) { put_num(size); out[outn++] = ' '; }
    put_num(ms); out[outn++] = '\n';
    int fd = s->f_open("CACHE.TXT", 0xB);
    if (fd >= 0) { s->f_write(fd, out, outn); s->f_close(fd); }
}

typedef u32 (*code_t)(void);

void *hx_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx)
{
    if (cmd == HX_FRAME) { s->present(); return keys ? 0 : ctx; }   /* the work is done in INIT; any key then stops the program */
    if (cmd != HX_INIT) return 0;
    static const u32 dsets[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 24, 32, 64 };
    static const u32 isets[] = { 1, 2, 4, 6, 8, 12, 16, 24, 32, 64 };
    u8 *fb = *s->display_buffer;
    for (u32 i = 0; i < (u32)*s->width * *s->height; i++) fb[i] = 7;       /* black while it runs */
    s->present();

    u32 sink = 0, t;
    t = s->now_ms(); for (u32 i = 0; i < 16 / DIV + 1; i++) { sink += alu_loop(1000000); s->wdt_kick(); } line(s, "alu", 0, s->now_ms() - t);

    u8 *buf = s->malloc(64 * 1024 + 64);
    if (!buf) return 0;
    for (u32 i = 0; i < 64 * 1024 / 4; i++) ((u32 *)buf)[i] = i;
    dcache_writeback(buf, 64 * 1024);
    for (u32 i = 0; i < sizeof dsets / sizeof dsets[0]; i++) {
        u32 ws = dsets[i] * 1024, total = 8u * 1024 * 1024 / DIV;
        sink += read_ws((volatile u32 *)buf, ws, ws, s);                     /* warm up the cache */
        t = s->now_ms(); sink += read_ws((volatile u32 *)buf, ws, total, s); line(s, "dread", dsets[i], s->now_ms() - t);
    }
    for (u32 i = 0; i < sizeof dsets / sizeof dsets[0]; i++) {
        u32 ws = dsets[i] * 1024, total = 8u * 1024 * 1024 / DIV;
        write_ws((volatile u32 *)buf, ws, ws, s);
        t = s->now_ms(); write_ws((volatile u32 *)buf, ws, total, s); line(s, "dwrite", dsets[i], s->now_ms() - t);
    }
    for (u32 i = 0; i < sizeof isets / sizeof isets[0]; i++) {
        u32 bytes = isets[i] * 1024, n = bytes / 4 - 1;                       /* n x l.addi r3,r3,1, then l.jr r9 */
        volatile u32 *c = (volatile u32 *)buf;
        for (u32 k = 0; k < n; k++) c[k] = 0x9C630001u;
        c[n] = 0x44004800u;
        dcache_writeback(buf, bytes);
        icache_reset();
        code_t f = (code_t)buf;
        u32 calls = 32u * 1000 * 1000 / DIV / n;
        sink += f();                                                          /* warm up */
        t = s->now_ms();
        for (u32 k = 0; k < calls; k++) { sink += f(); if ((k & 127) == 0) s->wdt_kick(); }
        line(s, "icode", isets[i], s->now_ms() - t);
    }
    s->free(buf);
    line(s, "done", 0, sink & 0);                          /* uses sink so the loops cannot be optimised away */
    for (u32 i = 0; i < (u32)*s->width * *s->height; i++) fb[i] = 0x0F;      /* a different colour when finished */
    s->present();
    return (void *)s;
}
