/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN (hxsdk, game host): sound test, following docs/analysis/sound.md (read from the disassembly, then tested). Steps:
   1. the stock key beep through the firmware's player 0x060385AC(3, no wait, volume 80), stopped after 600 ms;
   2. a 440 Hz tone, 1 s, 11025 Hz PCM16 mono, through the direct call 0x06027804(rate, buf, bytes), logging every audio callback event
      (bits 1/2 = streaming refill, 4 = done, per the analysis) with its time;
   3. the same tone as two 0.5 s buffers, the second started on the 'done' event: the gap between them;
   4. DSPISTOL from DOOM1.WAD (Doom's 8-bit unsigned format, 11025 Hz) converted to PCM16 and played the same way;
   5. start one half and queue the other at once with 0x02002B38 (next address/count): does the engine chain without a click?
   6. stream four quarter-second buffers, each queued from the callback on bit 2 (bit 2 then bit 4 arrive at each buffer end).
   Results: SOUND.TXT (written after each step). Screen: one bar per step (white while running, yellow done, red/orange failed). */
#include "services.h"
typedef int  (*play_res_t)(u32 id, u32 wait, u32 volume);
typedef void (*stop_res_t)(void);
typedef int  (*start_t)(u32 rate, const void *pcm, u32 bytes);
typedef void (*v_t)(void);
typedef void (*vol_t)(u32);
typedef void (*cbreg_t)(void (*)(u32));
typedef void (*wb_t)(void *, u32);
typedef int  (*seek_t)(int, u32, u32, u32);
typedef void (*queue_t)(const void *, u32);
#define FW_PLAY_RES ((play_res_t)0x060385AC)
#define FW_STOP_RES ((stop_res_t)0x06038554)
#define FW_START    ((start_t)0x06027804)
#define FW_STOP     ((v_t)0x06027860)
#define FW_VOLUME   ((vol_t)0x06027888)
#define FW_CALLBACK ((cbreg_t)0x060278E8)
#define FW_WB       ((wb_t)0x02004524)
#define FW_SEEK     ((seek_t)0x0601D018)
#define FW_QUEUE    ((queue_t)0x02002B38)         /* RAM copy of flash 0x5938: next DMA address (SPR 0x9148) and count (0x9158) */
#define CB_PTR      (*(u32 *)0x02012D8C)              /* the firmware's own audio callback, restored at the end */

static const struct hx_services *S;
static char lb[2048]; static u32 ll;
static void ps(const char *s) { while (*s && ll < sizeof lb - 1) lb[ll++] = *s++; }
static void pd(u32 v) { char t[12]; int i = 0; do t[i++] = (char)('0' + v % 10); while (v /= 10); while (i && ll < sizeof lb - 1) lb[ll++] = t[--i]; }
static void ph(u32 v) { for (int i = 28; i >= 0; i -= 4) if (ll < sizeof lb - 1) lb[ll++] = "0123456789abcdef"[(v >> i) & 15]; }
static void flush_log(void)
{
    static int created;
    int fd = created ? S->f_open("SOUND.TXT", 3) : S->f_open("SOUND.TXT", 0xB);
    if (fd >= 0) { created = 1; FW_SEEK(fd, S->f_size(fd), 0, 0); S->f_write(fd, lb, ll); S->f_close(fd); }
    ll = 0;
}
static void bar(int step, u8 col)
{
    u8 *fb = *S->display_buffer;
    for (u32 y = 30 + 32 * step; y < 52 + 32 * step; y++) for (u32 x = 20; x < 300; x++) fb[y * 320 + x] = col;   /* 6 bars, rows 30-211 */
    S->present();
}

/* audio callback (interrupt context): record each event's bits and time */
#define NEV 64
static volatile u32 ev_bits[NEV], ev_t[NEV], nev, done_flag;
/* stream mode (step 6): on bit 2 queue the next buffer while any are left; stop only on bit 4 after the last one */
static const char *volatile q_buf; static volatile u32 q_left, q_bytes, n4;
static void audio_event(u32 bits)
{
    u32 i = nev;
    if (i < NEV) { ev_bits[i] = bits; ev_t[i] = S->now_ms(); nev = i + 1; }
    if ((bits & 2) && q_left) { FW_QUEUE(q_buf, q_bytes); q_buf += q_bytes; q_left--; }
    if (bits & 4) { n4++; if (!q_left) { FW_STOP(); done_flag = 1; } }   /* stop as the firmware's callback 0x0602FDD0 does (not stopping rebooted the camera) */
}
static void log_events(u32 t0)
{
    ps("  events:"); for (u32 i = 0; i < nev && i < NEV; i++) { ps(" "); pd(ev_bits[i]); ps("@"); pd(ev_t[i] - t0); }
    ps(nev ? "\n" : " none\n");
}
static int wait_done(u32 ms) { u32 t = S->now_ms(); while (!done_flag && S->now_ms() - t < ms) S->wdt_kick(); return done_flag; }

/* read n bytes at file offset off: from the sector boundary into a 64-byte-aligned buffer (a read starting mid-sector is copied by the
   CPU and then partly overwritten by the SD driver's invalidate + DMA); seek returns < 0 on failure */
static u8 *read_at(int fd, u32 off, u32 n)
{
    u32 a = off & ~511u, skip = off - a, want = (skip + n + 511) & ~511u;
    u8 *b = S->malloc(want + 64);
    if (!b) return 0;
    if (FW_SEEK(fd, a, 0, 0) < 0 || S->f_read(fd, b, want) < (int)(skip + n)) { S->free(b); return 0; }
    if (skip) for (u32 i = 0; i < n; i++) b[i] = b[skip + i];
    return b;
}

static short *tone(u32 n, u32 rate, u32 hz)    /* square-ish wave via a triangle, no maths library */
{
    short *b = S->malloc(n * 2);
    if (!b) return 0;
    u32 period = rate / hz;
    for (u32 i = 0; i < n; i++) { u32 ph = i % period; int v = (int)(ph < period / 2 ? ph : period - ph) * 4 * 6000 / (int)period - 6000; b[i] = (short)v; }
    FW_WB(b, n * 2);
    return b;
}

void *hx_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx)
{
    (void)keys;
    if (cmd != HX_INIT) return cmd == HX_FRAME ? ctx : 0;
    S = s;
    u32 old_cb = CB_PTR;
    ps("hx330x soundtest v3\nold callback "); ph(old_cb); ps("\n"); flush_log();

    /* 1: stock key beep */
    bar(0, 2);
    u32 t0 = s->now_ms();
    int r = FW_PLAY_RES(3, 0, 80);                     /* no wait: its wait polls the firmware audio task, which may need the main loop */
    ps("1 stock beep (resource 3, no wait): returned "); pd((u32)r); ps(" after "); pd(s->now_ms() - t0); ps(" ms\n");
    { u32 t = s->now_ms(); while (s->now_ms() - t < 600) s->wdt_kick(); }
    FW_STOP_RES();
    flush_log(); bar(0, r == 0 ? 1 : 3);

    /* 2: 1 s tone through the direct call */
    bar(1, 2);
    const u32 RATE = 11025, N = 11024;                 /* multiple of 2 samples -> bytes multiple of 4 */
    short *b1 = tone(N, RATE, 440);
    if (!b1) { ps("2 malloc failed\n"); flush_log(); bar(1, 3); return (void *)1; }
    FW_STOP(); nev = 0; done_flag = 0;
    FW_CALLBACK(audio_event); FW_VOLUME(80);          /* (the stop sets the volume to 0: set it before every start) */
    t0 = s->now_ms();
    r = FW_START(RATE, b1, N * 2);
    int ok = wait_done(3000);
    ps("2 tone 1 s @11025: start returned "); pd((u32)r); ps(", done "); pd((u32)ok); ps(" after "); pd(s->now_ms() - t0); ps(" ms\n");
    log_events(t0); flush_log(); bar(1, r == 1 && ok ? 1 : 3);

    /* 3: two 0.5 s buffers back to back */
    bar(2, 2);
    nev = 0; done_flag = 0;
    t0 = s->now_ms();
    FW_VOLUME(80);
    r = FW_START(RATE, b1, N);                         /* first half of the tone */
    ok = wait_done(2000);
    u32 t_done = s->now_ms();
    done_flag = 0;
    FW_VOLUME(80);
    int r2 = FW_START(RATE, (char *)b1 + N, N);        /* second half */
    u32 t_restart = s->now_ms();
    int ok2 = wait_done(2000);
    ps("3 two buffers: starts "); pd((u32)r); ps("/"); pd((u32)r2); ps(", done "); pd((u32)ok); ps("/"); pd((u32)ok2);
    ps(", restart took "); pd(t_restart - t_done); ps(" ms, total "); pd(s->now_ms() - t0); ps(" ms (expected ~1000)\n");
    log_events(t0); flush_log(); bar(2, r == 1 && r2 == 1 && ok && ok2 ? 1 : 3);

    /* 4: DSPISTOL from the WAD */
    bar(3, 2);
    int fd = s->f_open("DOOM1.WAD", 1), played = 0;
    u8 *hd = fd >= 0 ? read_at(fd, 0, 12) : 0;
    u32 nl = hd ? (hd[4] | hd[5] << 8 | hd[6] << 16 | (u32)hd[7] << 24) : 0, io = hd ? (hd[8] | hd[9] << 8 | hd[10] << 16 | (u32)hd[11] << 24) : 0;
    u8 *dir = nl && nl <= 4096 ? read_at(fd, io, nl * 16) : 0;
    ps("4 WAD: "); pd(nl); ps(" lumps, directory "); ps(dir ? "read\n" : "NOT read\n");
    for (u32 i = 0; dir && i < nl; i++) {
        const u8 *e = dir + 16 * i; const char *nm = (const char *)e + 8;
        if (!(nm[0] == 'D' && nm[1] == 'S' && nm[2] == 'P' && nm[3] == 'I' && nm[4] == 'S' && nm[5] == 'T' && nm[6] == 'O' && nm[7] == 'L')) continue;
        u32 off = e[0] | e[1] << 8 | e[2] << 16 | (u32)e[3] << 24, len = e[4] | e[5] << 8 | e[6] << 16 | (u32)e[7] << 24;
        u8 *raw = len > 8 && len < 65536 ? read_at(fd, off, len) : 0;
        if (raw) {
            u32 rate = raw[2] | raw[3] << 8, n = raw[4] | raw[5] << 8 | raw[6] << 16 | (u32)raw[7] << 24;
            if (n > len - 8) n = len - 8;
            n &= ~1u;
            short *pcm = s->malloc(n * 2 + 64);
            if (pcm) {
                for (u32 k = 0; k < n; k++) pcm[k] = (short)(((int)raw[8 + k] - 128) << 8);
                FW_WB(pcm, n * 2);
                nev = 0; done_flag = 0; q_left = 0; t0 = s->now_ms();
                FW_VOLUME(80);
                r = FW_START(rate, pcm, n * 2);
                ok = wait_done(3000);
                ps("4 DSPISTOL: rate "); pd(rate); ps(", "); pd(n); ps(" samples, start "); pd((u32)r); ps(", done "); pd((u32)ok);
                ps(" after "); pd(s->now_ms() - t0); ps(" ms\n");
                log_events(t0);
                played = r == 1 && ok;
                s->free(pcm);
            }
            s->free(raw);
        }
        break;
    }
    if (dir) s->free(dir);
    if (hd) s->free(hd);
    if (fd >= 0) s->f_close(fd);
    if (!played) ps("4 DSPISTOL: not played\n");
    flush_log(); bar(3, played ? 1 : 3);

    /* 5: start the first half, queue the second at once (does the engine chain to the queued buffer, without a click?) */
    bar(4, 2);
    nev = 0; done_flag = 0; n4 = 0; q_left = 0;
    t0 = s->now_ms();
    FW_VOLUME(80);
    r = FW_START(RATE, b1, N);
    FW_QUEUE((char *)b1 + N, N);
    ok = wait_done(2500);
    ps("5 start + queue at once: start "); pd((u32)r); ps(", done "); pd((u32)ok); ps(" after "); pd(s->now_ms() - t0); ps(" ms (one buffer ~500, chained ~1000)\n");
    log_events(t0); flush_log(); bar(4, r == 1 && ok ? 1 : 3);
    FW_STOP();

    /* 6: stream: four quarter-second buffers, each next one queued from the callback on bit 2 */
    bar(5, 2);
    nev = 0; done_flag = 0; n4 = 0;
    q_bytes = N / 2; q_buf = (const char *)b1 + N / 2; q_left = 3;
    t0 = s->now_ms();
    FW_VOLUME(80);
    r = FW_START(RATE, b1, N / 2);
    ok = wait_done(2500);
    if (!ok) FW_STOP();
    ps("6 stream 4 x 250 ms, queued on bit 2: start "); pd((u32)r); ps(", done "); pd((u32)ok); ps(" after "); pd(s->now_ms() - t0);
    ps(" ms (~1000 if chained), bit-4 events "); pd(n4); ps(", left "); pd(q_left); ps("\n");
    log_events(t0); flush_log(); bar(5, r == 1 && ok ? 1 : 3);
    q_left = 0;

    FW_STOP();
    FW_CALLBACK((void (*)(u32))old_cb);              /* give the audio engine back to the firmware */
    s->free(b1);
    ps("done\n"); flush_log();
    return (void *)1;
}
