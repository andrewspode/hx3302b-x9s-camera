/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 Andrew Spode */
/* doomgeneric platform for the HX330x camera, run by the game host through the hxsdk (relocating crt0).
   Screen: Doom's 320x200 8-bit frame is drawn straight into the 320x240 display layer (rows 20..219); palette -> set_palette (alpha5|RGB565).
   Keys: one key at a time on this camera: UP/DOWN walk while held, LEFT/RIGHT turn, OK (shutter) fires, power tap = next weapon, power hold = Escape;
   use is automatic when walking into something (auto_use). In menus OK = Enter, UP/DOWN move the cursor. Time: firmware ms clock. Memory: newlib malloc over a firmware-malloc'd arena (hx_syscalls.c). */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "services.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doomstat.h"
#include "d_player.h"

const struct hx_services *hx_svc;
extern int hx_heap_init(const struct hx_services *s);

#define QN 32
static unsigned char q_key[QN], q_down[QN];
static unsigned q_head, q_tail;
static u32 prev_keys;

static void push(unsigned char key, int down) { unsigned n = (q_head + 1) % QN; if (n != q_tail) { q_key[q_head] = key; q_down[q_head] = (unsigned char)down; q_head = n; } }

/* Keys (one at a time on this camera): shutter, up, down, left, right, and the power button, whose tap/hold the key driver decides: tap ->
   HX_KEY_TAP, hold -> HX_KEY_ESC (each one frame); there is no hold-to-power-off (Quit powers off). Game: up/down walk while held, left/right turn, shutter
   fire, power tap = next weapon, power hold = Escape (menu); use (doors, switches) is pressed once when walking into something ("bump to use", auto_use).
   Each key releases what it pressed, even if a menu opened or closed in between (else fire could stay held after the menu closes). */
static const unsigned char game_map[7] = { KEY_FIRE, KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW, KEY_ESCAPE, ']' };
static const unsigned char menu_map[7] = { KEY_ENTER, KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW, KEY_ESCAPE, KEY_BACKSPACE };
static unsigned char down_key[7];
static void keys_changed(u32 now)
{
    u32 pressed = now & ~prev_keys, released = prev_keys & ~now;
    prev_keys = now;
    for (int b = 0; b < 7; b++) {
        if (released >> b & 1 && down_key[b]) { push(down_key[b], 0); down_key[b] = 0; }
        if (pressed >> b & 1 && (menuactive ? menu_map : game_map)[b]) { down_key[b] = (menuactive ? menu_map : game_map)[b]; push(down_key[b], 1); }
    }
}
static unsigned char pulse;                    /* a key pressed for one frame (use, next weapon): released on the next frame */
/* Use (doors, switches, lifts), "bump to use": pressed ONCE when walking forward but not actually moving (walked into something) for
   BUMP_MS, then not again until the player has moved on. (Pressing use repeatedly while walking would re-use a door that is already
   opening, closing it again.) */
#define BUMP_MS 150
#define BUMP_MOVED (2 * FRACUNIT)                 /* map units: moved this far since the last check point = not blocked */
static void auto_use(u32 t)                    /* called every frame, after keys_changed */
{
    static fixed_t lx, ly;
    static u32 t_still;
    static int used;
    if (pulse == KEY_USE) { push(KEY_USE, 0); pulse = 0; }       /* (other one-frame keys come as one-frame bits from the host) */
    mobj_t *mo = players[consoleplayer].mo;
    if (menuactive || gamestate != GS_LEVEL || !mo || down_key[1] != KEY_UPARROW) {
        used = 0; t_still = t; if (mo) { lx = mo->x; ly = mo->y; }
        return;
    }
    fixed_t dx = mo->x - lx, dy = mo->y - ly;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx + dy > BUMP_MOVED) { lx = mo->x; ly = mo->y; t_still = t; used = 0; }   /* moving: not blocked */
    else if (!used && !pulse && t - t_still >= BUMP_MS) { push(KEY_USE, 1); pulse = KEY_USE; used = 1; }
}

/* Doom's zone: BORROWED firmware static RAM, 0x0201C000-0x02096FFF (492 KiB). watching RAM showed 0x0201B000-0x02096FFF untouched in a
   game window, but the same with file I/O showed 0x0201B000-0x0201BFFF is the file system's buffer (the camera rebooted while
   reading the WAD with the zone starting there). With the zone at 0x0201C000 Doom failed 'Z_Malloc: failed on allocation of 40 bytes'
   right after opening the WAD: something wrote into the zone's first block header, probably the file system's long-file-name handling
   (.default.cfg). With a guard page at 0x0201C000 and the zone at 0x0201D000, the whole guard page changed after every log write and the zone failed the
   same way. With 36 written-back guard pages 0x0201C000-0x0203FFFF and the zone at 0x02040000, guard pages 0-6 (0x0201C000-0x02022FFF) were
   overwritten after EVERY file operation (a DMA that keeps writing the same values, invisible to checksum-based RAM watching); pages 7-35 never
   changed. Doom finished its whole start-up and ran out of zone (348 KiB). So: avoid 0x0201B000-0x02022FFF; zone 0x02023000-0x02096FFF. The firmware may need this RAM in other modes: reboot the camera after playing. */
#define HX_ZONE_BASE ((byte *)0x02023000)          /* to 0x02096FFF: 464 KiB */
#define HX_ZONE_SIZE 0x74000
#ifdef HX_FLASH
/* Flash-resident build: Doom's cold code runs in place from flash (0x062A0000 up), the rest (hot code, read-only data, data, bss) is linked at
   fixed RAM 0x02023000 (the borrowed area above) and big renderer arrays at 0x020F5000; the host copies and zeroes them from the header
   below. The zone is the firmware heap's largest block (no program image there now) plus the left-over ends of both RAM areas. */
u8 *hx_fw_zone(int *size);
byte *hx_zone(int *size)
{
    byte *z = hx_fw_zone(size);
    if (!z) { printf("hx330x: no heap for the zone\n"); *size = 0; }
    return z;
}
extern char __text_dst[], __text_src[], __text_len[], __data_dst[], __data_src[], __data_len[];
extern char __bss_start[], __bss_len[], __bss2_start[], __bss2_len[], __bss_end[], __bss2_end[], __flash_end[];
struct hx_fprog { u32 magic, version, entry, copy[2][3], zero[2][2], flash_end; };
void *hx_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx);
__attribute__((section(".fhdr"), used)) const struct hx_fprog hx_fprog = {
    0x4D445848u /* 'HXDM' */, 1, (u32)hx_main,
    { { (u32)__text_dst, (u32)__text_src, (u32)__text_len }, { (u32)__data_dst, (u32)__data_src, (u32)__data_len } },
    { { (u32)__bss_start, (u32)__bss_len }, { (u32)__bss2_start, (u32)__bss2_len } }, (u32)__flash_end };
#else
byte *hx_zone(int *size)
{
    *size = HX_ZONE_SIZE;
    return HX_ZONE_BASE;
}
#endif

/* Visible state, so a slow load and a hang can be told apart. Before Doom sets its palette the stock palette is active (2 = white). */
static int doom_palette_set;
/* HX_STRETCH: Doom's 320x200 frame is shown stretched to the layer's 320x240, which is Doom's intended aspect ratio
   (its 200 lines filled a 4:3 screen); otherwise it is drawn in place with black 20-row bands. Stretching from a separate 320x200
   buffer would cost 64000 bytes of zone (a 35 KB texture then failed to allocate), so it stretches IN PLACE: Doom draws into the layer's rows
   0-199, DG_DrawFrame spreads them over 240 rows from the bottom up, and patch 20 makes Doom redraw its status bar and view border every
   frame (its only partial redraws), since the layer no longer holds the previous 320x200 frame. */
#ifndef HX_STRETCH
#define HX_STRETCH 1
#endif
#define BAR_Y 0                                     /* loading bar: the top 6 rows (at the bottom it got in the way) */
static int title_shown, bar_col = 2;                /* bar_col: Doom's reddest palette entry once PLAYPAL is loaded (show_title) */
static void bar(u32 w)                              /* red for the done part, black for the rest; the next frame draws over it */
{
    u8 *fb = *hx_svc->display_buffer;
    if (w > 320) w = 320;
    for (u32 y = BAR_Y; y < BAR_Y + 6; y++) for (u32 x = 0; x < 320; x++) fb[y * 320 + x] = x < w ? (u8)bar_col : 0;
    hx_svc->present();
}
void hx_progress(int step) { if (!doom_palette_set) bar(8 * (u32)step); }   /* start-up: one step per log line */
void hx_loading(int step, int of) { bar(320 * (u32)step / (u32)of); }       /* level loading (p_setup.c patch 18): six steps */

/* While Doom starts (it takes a while), show TITLEPIC with Doom's palette, read straight from the WAD. Done before Doom opens the WAD (a
   second open while Doom holds it crashes the firmware's file layer) and everything is freed and closed again. */
/* Scratch: the big renderer arrays' area (bss2, 0x020F5000 +0x13000), unused until R_Init; zeroed again afterwards (Doom expects zeroed bss).
   No heap use: the newlib arena is small, and freed firmware-heap blocks could leave a hole before Doom's zone. */
#ifdef HX_FLASH
extern char __bss2_start[], __bss2_end[];
#define SCRATCH ((u8 *)__bss2_start)
#define SCRATCH_N ((size_t)(__bss2_end - __bss2_start))
#else
static u8 scratch_buf[70000];
#define SCRATCH scratch_buf
#define SCRATCH_N sizeof scratch_buf
#endif
static void *wad_read(int fd, long off, size_t n, size_t at)
{
    if (at + n > SCRATCH_N) return 0;
    u8 *b = SCRATCH + at;
    return lseek(fd, off, SEEK_SET) == off && read(fd, b, n) == (int)n ? b : 0;
}
static void show_title(void)
{
    int fd = open("DOOM1.WAD", O_RDONLY);
    if (fd < 0) return;
    int hdr[3];
    u8 *dir = 0, *pal = 0, *pic = 0;
    if (read(fd, hdr, 12) != 12 || hdr[1] <= 0 || hdr[1] > 4096 || !(dir = wad_read(fd, hdr[2], (size_t)hdr[1] * 16, 0))) goto out;
    int p_off = -1, p_len = 0, t_off = -1, t_len = 0;
    for (int i = 0; i < hdr[1]; i++) {
        const u8 *e = dir + 16 * i; int off, len; memcpy(&off, e, 4); memcpy(&len, e + 4, 4);
        if (!strncmp((const char *)e + 8, "PLAYPAL", 8)) { p_off = off; p_len = len; }
        if (!strncmp((const char *)e + 8, "TITLEPIC", 8)) { t_off = off; t_len = len; }
    }
    if (p_off < 0 || p_len < 768 || t_off < 0 || t_len < 8 || !(pal = wad_read(fd, p_off, 768, 0)) || !(pic = wad_read(fd, t_off, (size_t)t_len, 768))) goto out;
    short w, h; memcpy(&w, pic, 2); memcpy(&h, pic + 2, 2);
    if (w != 320 || h != 200 || t_len < 8 + 4 * w) goto out;
    u8 *fb = *hx_svc->display_buffer;
    memset(fb, 0, 320 * 240);
    for (int x = 0; x < w; x++) {                  /* patch format: per column, posts {topdelta, length, pad, data[length], pad} until 0xFF */
        int co; memcpy(&co, pic + 8 + 4 * x, 4);
        while (co >= 0 && co + 3 < t_len && pic[co] != 0xFF) {
            int top = pic[co], len = pic[co + 1];
            if (co + 4 + len > t_len) break;
            for (int k = 0; k < len && top + k < 200; k++) {
                u32 r = (u32)(top + k);
                if (HX_STRETCH) for (u32 y = r * 6 / 5; y < (r + 1) * 6 / 5; y++) fb[y * 320 + x] = pic[co + 3 + k];   /* rows 200 -> 240 */
                else fb[(20 + r) * 320 + x] = pic[co + 3 + k];
            }
            co += len + 4;
        }
    }
    static u32 p32[256];
    int best = -1000;
    for (int i = 0; i < 256; i++) {
        u32 r = pal[3 * i], g = pal[3 * i + 1], b = pal[3 * i + 2];
        p32[i] = (0x1Fu << 16) | (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
        if (!(p32[i] & 0xFFFF)) p32[i] |= 1;       /* as in DG_DrawFrame: no pure black */
        if ((int)r - (int)(g + b) > best) { best = (int)r - (int)(g + b); bar_col = i; }   /* the reddest entry */
    }
    hx_svc->set_palette(0, p32);
    hx_svc->present();
    title_shown = 1;
out:
    (void)dir;
    memset(SCRATCH, 0, SCRATCH_N);
    close(fd);
}
void hx_fatal(void)                          /* solid red: everything palette 0 = opaque red */
{
    { extern void hx_sound_stop(void), hx_log_flush(void); hx_sound_stop(); fflush(stdout); fflush(stderr); hx_log_flush(); }
    static u32 red[256];
    for (int i = 0; i < 256; i++) red[i] = (0x1Fu << 16) | (0x1Fu << 11);
    hx_svc->set_palette(0, red);
    memset(*hx_svc->display_buffer, 0, 320 * 240);
    hx_svc->present();
}

/* Extra zone regions, each pattern-tested first: fill, write back to RAM, do some file reads, wait, verify (this found a 28 KiB area that a
   DMA keeps rewriting with the same values: only a written pattern shows that). Plus our own relocation table, dead after crt0. */
void Z_AddRegion(byte *base, int size);
int hx_io_stress(void);                                 /* hx_syscalls.c */
typedef void (*wb_t)(void *, u32);
static int region_ok(u32 a, u32 n)
{
    volatile u32 *p = (volatile u32 *)a;
    for (u32 i = 0; i < n / 4; i++) p[i] = (a + 4 * i) ^ 0x5AC3E10Fu;
    ((wb_t)0x02004524)((void *)a, n);
    hx_io_stress();
    u8 *fb = *hx_svc->display_buffer;                  /* a crash right after start-up with these regions in use; the blocks are
                                                          screen-buffer sized, so exercise the display path as a running game does */
    for (u32 k = 0; k < 20; k++) { memset(fb + 12 * 320, (int)(k * 13), 320 * 228); hx_svc->present(); }
    memset(fb + 12 * 320, 0, 320 * 228);
    u32 t = hx_svc->now_ms(); while (hx_svc->now_ms() - t < 500) hx_svc->wdt_kick();
    u32 bad = 0;
    for (u32 i = 0; i < n / 4; i++) if (p[i] != ((a + 4 * i) ^ 0x5AC3E10Fu)) bad++;
    printf("hx330x zone region %08lx +%lx: %s (%lu bad words)\n", (unsigned long)a, (unsigned long)n, bad ? "REJECTED" : "ok", (unsigned long)bad);
    return !bad;
}
#ifndef HX_FLASH
extern char _start[];
#endif
void hx_zone_extra(void)
{
    /* Testing showed ALL of these blocks are display buffers: after the first full-screen draw the firmware keeps copying the
       screen into them. Never use them (HX_REGIONS stays 0); kept only to record what was tried. */
    static const u32 cand[][2] = { { 0x020BE000, 0x11000 }, { 0x020D9000, 0x9000 }, { 0x020E3000, 0x26000 } };
    /* HX_REGIONS: bitmask of candidate regions to test and use (none are used) */
#ifndef HX_REGIONS
#define HX_REGIONS 0
#endif
    for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++)
        if ((HX_REGIONS >> i) & 1 && region_ok(cand[i][0], cand[i][1])) Z_AddRegion((byte *)cand[i][0], (int)cand[i][1]);
    /* Testing showed 0x020F5000-0x02108FFF stayed untouched through draws, palette, SD reads/writes and 40 s idle; it lies between
       the last display copy (ends 0x020F4F40) and the heap (0x02109600). (The 112 KiB above the heap is the firmware's: writing it rebooted.) */
#ifndef HX_FLASH                                           /* (flash-resident build: this area holds the big renderer arrays; its rest is added below) */
    printf("hx330x zone region 020f5000 +14000: memtest v3 clean\n");
    Z_AddRegion((byte *)0x020F5000, 0x14000);
#endif
#ifdef HX_FLASH
    { extern u8 *hx_fw_small(int *); int n; u8 *sm = hx_fw_small(&n);   /* added FIRST: the small-allocation rover starts in it */
      printf("hx330x zone region %08lx +%x: small allocations\n", (unsigned long)sm, n); if (sm) Z_AddRegion(sm, n); }
    u32 from[2] = { ((u32)__bss_end + 15) & ~15u, ((u32)__bss2_end + 15) & ~15u }, to[2] = { 0x02097000, 0x02109000 };  /* left-over tops */
    for (int i = 0; i < 2; i++) {
        printf("hx330x zone region %08lx +%lx: rest of fixed RAM area %d\n", (unsigned long)from[i], (unsigned long)(to[i] - from[i]), i);
        if (to[i] > from[i] + 4096) Z_AddRegion((byte *)from[i], (int)(to[i] - from[i]));
    }
#else
    const u32 *h = (const u32 *)(_start + 4);                /* crt0 header: table offset, count (8 bytes each) */
    u32 t0 = ((u32)_start + h[1] + 15) & ~15u, t1 = ((u32)_start + h[1] + h[2] * 8) & ~15u;
    printf("hx330x zone region %08lx +%lx: relocation table\n", (unsigned long)t0, (unsigned long)(t1 - t0));
    Z_AddRegion((byte *)t0, (int)(t1 - t0));
#endif
}

byte *hx_video_buffer(void)                     /* I_InitGraphics (patch 7): where Doom draws: the layer (rows 0-199, or 20-219 with bands) */
{
    return (byte *)DG_ScreenBuffer;
}
void DG_Init(void)
{
    free(DG_ScreenBuffer);                                  /* draw straight into the display layer, 20 rows down */
    u8 *layer = *hx_svc->display_buffer;
    if (!title_shown) memset(layer, 0, 320 * 240);          /* (keep the loading title) */
    DG_ScreenBuffer = HX_STRETCH ? layer : layer + 20 * 320;
}

/* profiling: ms per phase, accumulated; [0] game tics, [1] 3D view, [2] status bar + HUD, [3] present (palette + bands + present),
   [4] sound mixing, [5] SD read time, [6] SD read bytes, [7] SD read calls. Patch 13 times the Doom calls; the platform files the rest. */
unsigned hx_prof[8];
unsigned hx_now(void) { return hx_svc->now_ms(); }

void DG_DrawFrame(void)
{
    u32 t_draw = hx_svc->now_ms();
    static u32 draws;
    int dbg = 0; ++draws;                      /* crashes occurred right after a draw log line, before set_palette: no logging around set_palette/present */
    if (dbg) printf("hx330x draw %lu (palette_changed %d)\n", (unsigned long)draws, palette_changed);
    /* (a TITLEPIC memory-vs-file check here crashed the camera every time: it opened DOOM1.WAD a second time while Doom has it open;
       the firmware's file layer does not cope. In the emulator memory and file matched. Removed.) */
    if (palette_changed) {
        static u32 pal[256];
        for (int i = 0; i < 256; i++)
            pal[i] = (0x1Fu << 16) | ((u32)(colors[i].r >> 3) << 11) | ((u32)(colors[i].g >> 2) << 5) | (colors[i].b >> 3);
        for (int i = 0; i < 256; i++)              /* pure black (RGB565 0) may be the display's transparent key (the bands showed magenta) */
            if (!(pal[i] & 0xFFFF)) pal[i] |= 1;
        hx_svc->set_palette(0, pal);
        palette_changed = 0;
        doom_palette_set = 1;
    }
    u8 *layer = *hx_svc->display_buffer;
    if (HX_STRETCH) {                          /* in place: layer row y shows Doom row y * 5 / 6, from the bottom up (source row <= y, unwritten yet) */
        for (int y = 239; y > 0; y--) {
            u32 sy = (u32)y * 5 / 6;
            if (sy == (u32)y) continue;
            const u32 *src = (const u32 *)(layer + sy * 320);
            u32 *dst = (u32 *)(layer + (u32)y * 320);
            for (int i = 0; i < 80; i++) dst[i] = src[i];
        }
    } else {                                   /* the 20-row bands above and below Doom's 200 rows: black (index 0) every frame */
        memset(layer, 0, 20 * 320);
        memset(layer + 220 * 320, 0, 20 * 320);
    }
    if (dbg) printf("hx330x draw %lu: present\n", (unsigned long)draws);
    hx_svc->present();
    hx_prof[3] += hx_svc->now_ms() - t_draw;
    if (dbg) printf("hx330x draw %lu: present done\n", (unsigned long)draws);
}

void DG_SleepMs(uint32_t ms) { u32 t = hx_svc->now_ms(); while (hx_svc->now_ms() - t < ms) hx_svc->wdt_kick(); }
uint32_t DG_GetTicksMs(void) { hx_svc->wdt_kick(); return hx_svc->now_ms(); }
int DG_GetKey(int *pressed, unsigned char *key)
{
    if (q_tail == q_head) return 0;
    *key = q_key[q_tail]; *pressed = q_down[q_tail]; q_tail = (q_tail + 1) % QN;
    return 1;
}
void DG_SetWindowTitle(const char *title) { (void)title; }

/* Doom's own stack, 32 KiB from the firmware heap. It used to be 0x020D1000-0x020D8FFF, which testing showed is a display buffer the
   firmware keeps overwriting with screen pixels (it caused crashes and probably a broken title). */
#define HX_STACK 0x8000u
static u32 stack_top;
void *hx_on_stack(const struct hx_services *, u32, u32, void *, u32 top, void *(*fn)(const struct hx_services *, u32, u32, void *));
static void *doom_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx);
void *hx_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx)
{
    if (cmd == HX_INIT) {
        u8 *st = s->malloc(HX_STACK);
        if (!st) return 0;
        stack_top = (u32)st + HX_STACK;
    }
    if (!stack_top) return 0;
    void *r = hx_on_stack(s, cmd, keys, ctx, stack_top, doom_main);
    if (cmd == HX_QUIT || (cmd == HX_INIT && !r)) { s->free((void *)(stack_top - HX_STACK)); stack_top = 0; }
    return r;
}

static u32 frame_no;
#ifdef HX_RELEASE                               /* release build: no per-frame statistics or slow-frame lines in the log */
static int log_frame(u32 n) { (void)n; return 0; }
#else
static int log_frame(u32 n) { return n <= 5 || n % 50 == 0; }
#endif
static void *doom_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx)
{
    static char *argv[] = { "doom", "-iwad", "DOOM1.WAD", 0 };
    if (cmd == HX_INIT) {
        hx_svc = s;
        if (s->version < 2 || hx_heap_init(s)) return 0;
        setvbuf(stdout, NULL, _IOLBF, 256);
        setvbuf(stderr, NULL, _IOLBF, 256);
        show_title();
        printf("hx330x doom: starting (t=%lu ms)%s\n", (unsigned long)s->now_ms(), title_shown ? ", title shown" : "");
        doomgeneric_Create(3, argv);
        { extern u32 hx_heap_used(void), hx_heap_size(void); printf("hx330x doom: init done (t=%lu ms; newlib heap %lu of %lu bytes used)\n", (unsigned long)s->now_ms(), (unsigned long)hx_heap_used(), (unsigned long)hx_heap_size()); }
        { extern int hx_log_buffered; hx_log_buffered = 1; }    /* from now on the log collects in RAM (writes cost ~110 ms) */
        return (void *)1;
    }
    if (cmd == HX_QUIT) { extern void hx_sound_stop(void), hx_log_flush(void); hx_sound_stop(); printf("hx330x doom: stopped by the host\n"); fflush(stdout); hx_log_flush(); return 0; }
    u32 t_keys = s->now_ms();
    if (keys != prev_keys) keys_changed(keys);
    auto_use(t_keys);
    frame_no++;
    static u32 doom_ms, t_last, worst, slow_logged, p0[8];
    u32 t_in = s->now_ms();
    if (log_frame(frame_no)) {
        printf("hx330x frame %lu start (gamestate %d, t=%lu ms; last 50: %lu ms in Doom of %lu, worst frame %lu ms; tics %u view %u hud %u present %u mix %u, SD %u ms %u KB in %u reads; audio underruns %d; zone static %d)\n",
               (unsigned long)frame_no, gamestate, (unsigned long)t_in, (unsigned long)doom_ms, (unsigned long)(t_last ? t_in - t_last : 0),
               (unsigned long)worst, hx_prof[0], hx_prof[1], hx_prof[2], hx_prof[3], hx_prof[4], hx_prof[5], hx_prof[6] >> 10, hx_prof[7],
               ({ extern int hx_sound_underruns(void); hx_sound_underruns(); }), ({ extern int Z_HxStaticBytes(void); Z_HxStaticBytes(); }));
        doom_ms = 0; t_last = t_in; worst = 0; slow_logged = 0;
        for (int i = 0; i < 8; i++) hx_prof[i] = 0;
    }
    for (int i = 0; i < 8; i++) p0[i] = hx_prof[i];
#ifdef HX_SHORT_DEMO                                       /* (emulator test: end the demo early to exercise the page / level-free path) */
    { extern int demoplayback; extern void G_CheckDemoStatus(void); if (frame_no == HX_SHORT_DEMO && demoplayback) G_CheckDemoStatus(); }
#endif
    doomgeneric_Tick();
    u32 dt = s->now_ms() - t_in;
    doom_ms += dt;
    if (dt > worst) worst = dt;
#ifdef HX_RELEASE
    if (0) {
#else
    if (dt > 80 && slow_logged < 3 && gamestate == 0) {
#endif           /* a slow frame in a level: what was it? (at most 3 per 50 frames) */
        slow_logged++;
        printf("hx330x slow frame %lu: %lu ms: tics %u view %u hud %u present %u mix %u, SD %u ms %u KB in %u reads\n", (unsigned long)frame_no,
               (unsigned long)dt, hx_prof[0] - p0[0], hx_prof[1] - p0[1], hx_prof[2] - p0[2], hx_prof[3] - p0[3], hx_prof[4] - p0[4],
               hx_prof[5] - p0[5], (hx_prof[6] - p0[6]) >> 10, hx_prof[7] - p0[7]);
    }
    { extern int hx_quit; extern void hx_sound_stop(void); if (hx_quit) { extern void hx_log_flush(void); hx_sound_stop(); printf("hx330x doom: quit (power off)\n"); fflush(stdout); hx_log_flush(); return 0; } }
    if (frame_no <= 3) printf("hx330x frame %lu done\n", (unsigned long)frame_no);
    return ctx;
}
