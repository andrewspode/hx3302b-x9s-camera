/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* Game host in the 'fly' slot (flash 0x4CBA0, runs in place at 0x0604CBA0). Once a program runs, the host drives its own main loop (see run()), reads the keys itself and times the power key; the program's Quit powers the camera off.
   Window open: load GAME.BIN from the SD card if there is one, otherwise start the program in flash (HXDM header at 0x062A0000:
   copy its RAM parts to their fixed addresses, zero its bss ranges, write the cache back, reset the instruction cache), call entry(HX_INIT),
   post the private frame event 0x0D. The first event 0x0D starts the host's own loop (see run()), which calls entry(HX_FRAME, keys, ctx) every frame. Keys 0x14-0x18 keep a bitmask (param 0 press sets, 2 release clears).
   Back/close: entry(HX_QUIT), free, then fly's own handler (fly's close frees the host state, whose pointer lives in fly's variable 0x02019358).
   Message queue: post(*(u32*)0x0201A014, id | param << 16) = fn_0602F36C. Event 0x0D is never posted by the firmware (checked statically). */
#include "services.h"

int  fw_f_open(const char *name, int mode);
u32  fw_f_size(int fd);
int  fw_f_read(int fd, void *buf, u32 len);
int  fw_f_write(int fd, const void *buf, u32 len);
void fw_f_close(int fd);
void *fw_malloc(u32);
void fw_free(void *);
void *fw_memcpy(void *, const void *, u32);
void fw_dcache_inval(void *addr, u32 len);              /* 0x060691EC: INVALIDATE (right after the SD DMA read in load()) */
void fw_dcache_wb(void *addr, u32 len);                 /* 0x02004524: write-back (after CPU copies of code) */
void fw_icache_reset(void);
void fw_present(void);
u32  fw_now_ms(void);
void fw_wdt_kick(void);
void fw_set_palette(u32 layer, const u32 *pal);
void fw_post(void *queue, u32 msg);
void fw_palette_restore(void);                          /* 0x0604196C */
int  fw_dev(int device, u32 command, void *arg);
void fw_mode_request(u32 mode, u32 arg);                /* 0x060472E8 */        /* 0x0601450C: device 3 'key', command 0x400 -> next packed key event */

static const struct hx_services svc = {
    HX_SERVICES_MAGIC, 2, fw_present, fw_now_ms, fw_wdt_kick, fw_memcpy, fw_malloc, fw_free,
    (u8 **)0x02019FF8, (u16 *)0x02019FF4, (u16 *)0x02019FF6,
    fw_set_palette, fw_f_open, fw_f_read, fw_f_write, fw_f_close, fw_f_size,
};

#define EV_FRAME   0x0D
#define STATE_PTR  ((struct host **)0x02019358)
#define HOST_MAGIC 0x54534F48u                              /* 'HOST' */
typedef void *(*prog_t)(const struct hx_services *, u32 cmd, u32 keys, void *ctx);
typedef int (*handler_t)(void *, int, int *);
struct host { u32 magic; void *code; void *ctx; u32 keys; u32 running; u32 in_flash; u32 need_init; u32 latch; };
struct fprog { u32 magic, version, entry, copy[2][3], zero[2][2], flash_end; };
#define FPROG ((const struct fprog *)0x062A0000)
#define FPROG_MAGIC 0x4D445848u                             /* 'HXDM' */
static int ram_ok(u32 a, u32 n) { return a >= 0x02011000 && a + n <= 0x02200000 && a + n >= a; }

static struct host *state(void)
{
    struct host *h = *STATE_PTR;
    u32 a = (u32)h;
    return (a >= 0x02000000 && a < 0x02200000 && !(a & 3) && h->magic == HOST_MAGIC) ? h : 0;
}
static void post_frame(void) { fw_post(*(void **)0x0201A014, EV_FRAME); }

static void *load(const char *name)
{
    int fd = fw_f_open(name, 1);
    if (fd < 0) return 0;
    u32 size = fw_f_size(fd);
    void *buf = 0;
    if (size >= 4 && size <= 0x100000) buf = fw_malloc(size);
    if (buf && fw_f_read(fd, buf, size) != (int)size) { fw_free(buf); buf = 0; }
    fw_f_close(fd);
    if (!buf) return 0;
    fw_dcache_inval(buf, size);
    fw_icache_reset();
    return buf;
}
static void *load_flash(void)
{
    const struct fprog *p = FPROG;
    if (p->magic != FPROG_MAGIC || p->version != 1 || p->flash_end > 0x06400000) return 0;
    for (int i = 0; i < 2; i++) {
        u32 dst = p->copy[i][0], src = p->copy[i][1], n = p->copy[i][2];
        if (!ram_ok(dst, n) || src < 0x062A0000 || src + n > p->flash_end || (dst | src | n) & 3) return 0;
    }
    for (int i = 0; i < 2; i++) if (!ram_ok(p->zero[i][0], p->zero[i][1]) || (p->zero[i][0] | p->zero[i][1]) & 3) return 0;
    /* None of the program's RAM may be FREE heap (the heap starts at 0x020F6A00 with the Games mode's screen
       buffer, which is what keeps Doom's area2 out of malloc's reach). Free list: *0x0201327C, descriptors {used, addr, size, -, next}. */
    u32 d = *(volatile u32 *)0x0201327C;
    for (int n = 0; d && n < 512; n++) {
        const volatile u32 *e = (const volatile u32 *)d;
        u32 fa = e[1], fn = e[2];
        for (int i = 0; i < 4; i++) {
            u32 a = i < 2 ? p->copy[i][0] : p->zero[i - 2][0], m = i < 2 ? p->copy[i][2] : p->zero[i - 2][1];
            if (m && a < fa + fn && fa < a + m) return 0;
        }
        d = e[4];
    }
    for (int i = 0; i < 2; i++) {
        volatile u32 *d = (volatile u32 *)p->copy[i][0];    /* CPU copy: a DMA read of flash returns zeros (USB dump work) */
        const volatile u32 *f = (const volatile u32 *)p->copy[i][1];
        for (u32 k = 0; k < p->copy[i][2] / 4; k++) d[k] = f[k];
        fw_dcache_wb((void *)p->copy[i][0], p->copy[i][2]);
        fw_wdt_kick();
    }
    for (int i = 0; i < 2; i++) {
        volatile u32 *z = (volatile u32 *)p->zero[i][0];
        for (u32 k = 0; k < p->zero[i][1] / 4; k++) z[k] = 0;
        fw_dcache_wb((void *)p->zero[i][0], p->zero[i][1]);
    }
    fw_icache_reset();
    return (void *)p->entry;
}
static void stop(struct host *h)
{
    if (h->code) {
        if (h->running) ((prog_t)h->code)(&svc, HX_QUIT, h->keys, h->ctx);
        if (!h->in_flash) fw_free(h->code);
        /* Give the screen back as the firmware set it up: clear the layer, reload the stock palette (0x0604196C: resource 0x40 ->
           set_palette(0, ...)); otherwise quitting with the power key left a purple screen (Doom's palette left on the layer) */
        volatile u32 *fb = *(volatile u32 **)0x02019FF8;
        for (u32 i = 0; i < 320 * 240 / 4; i++) fb[i] = 0;
        fw_dcache_wb((void *)fb, 320 * 240);
        fw_palette_restore();
    }
    h->code = 0; h->ctx = 0; h->running = 0;
}

int hx_stub(void *win, int n, int *param) { (void)win; (void)n; (void)param; return 0; }

int hx_open(void *win, int n, int *param)
{
    (void)win; (void)n; (void)param;
    struct host *h = fw_malloc(sizeof *h);
    *STATE_PTR = h;
    if (!h) return 0;
    h->magic = HOST_MAGIC; h->keys = 0; h->running = 0; h->ctx = 0; h->in_flash = 0; h->need_init = 0; h->latch = 0;
    h->code = load("GAME.BIN");
    /* A flash program is prepared (copied, zeroed, checked against the free heap) on the first frame event, NOT here: the Games
       mode's enter opens this window BEFORE it mallocs its screen buffer (0x020F6A00, = most of Doom's area2), so here that RAM is still
       free and the check would refuse Doom (the Games list and its music would just carry on). */
    if (!h->code && FPROG->magic == FPROG_MAGIC) h->in_flash = 1;
    /* HX_INIT on the first frame event, not inside the window open: with the Games button opening this window directly (patched mode-7
       enter), the enter function mallocs its screen-sized buffer AFTER the open; the program must not take the heap before that. */
    if (h->code || h->in_flash) { h->need_init = 1; post_frame(); }
    return 0;
}

/* The program's own loop. Measured: 38 ms per frame in the firmware's main loop between our frames (Doom: 42 ms), so the
   host no longer returns to it while a program runs: it reads the key device itself (as the UI poll 0x060493EC does), calls HX_FRAME,
   kicks the watchdog. The ms clock is interrupt-driven (0x0602F63C), drawing and SD access are synchronous calls. The loop ends when the
   program returns 0 (Doom: Quit); the camera then powers off (mode 1), which is the guaranteed clean restart. Key events: low 16 bits id 0x14-0x1A, high 16 bits 0 press / 1 repeat / 2 release. */
static const u32 key_bit[5] = { HX_KEY_OK, HX_KEY_UP, HX_KEY_DOWN, HX_KEY_LEFT, HX_KEY_RIGHT };   /* ids 0x14-0x18 */
/* Power key: the key driver decides tap vs hold itself. A TAP arrives as 0x19 press + release
   (20 ms apart) when the button is let go. A HOLD gives nothing at first, then 0x1A press once the driver is sure (~0.5-1 s), a repeat
   every 20 ms, and an explicit 0x1A release. So: 0x19 release -> HX_KEY_TAP (one frame); 0x1A press -> HX_KEY_ESC (one frame).
   There is no hold-to-power-off in the host (not needed: Doom's Quit powers off; a frozen program would not run this loop anyway). */
static void run(struct host *h)
{
    for (;;) {
        for (int i = 0; i < 8; i++) {
            u32 ev = 0;
            fw_dev(3, 0x400, &ev);
            if (!ev) break;
            u32 id = ev & 0xFFFF, p = ev >> 16;
            if (id == 0x19) { if (p == 2) h->latch |= HX_KEY_TAP; continue; }
            if (id == 0x1A) { if (p == 0) h->latch |= HX_KEY_ESC; continue; }
            if (id < 0x14 || id > 0x18) continue;
            u32 bit = key_bit[id - 0x14];
            if (p == 0) { h->keys |= bit; h->latch |= bit; }
            else if (p == 2) h->keys &= ~bit;
        }
        u32 k = h->keys | h->latch;
        h->latch = 0;
        h->ctx = ((prog_t)h->code)(&svc, HX_FRAME, k, h->ctx);
        fw_wdt_kick();
        if (!h->ctx) break;
    }
    stop(h);
    fw_mode_request(1, 0);                              /* 'Power Off' ("bye bye") */
}

int hx_frame(void *win, int n, int *param)
{
    (void)win; (void)n; (void)param;
    struct host *h = state();
    if (!h) return 0;
    if (h->need_init) {
        h->need_init = 0;
        if (h->in_flash && !h->code && !(h->code = load_flash())) { h->in_flash = 0; return 0; }   /* refused: the firmware carries on */
        h->ctx = ((prog_t)h->code)(&svc, HX_INIT, 0, 0);
        if (h->ctx) { h->running = 1; run(h); }
        return 0;
    }
    if (!h->running) return 0;
    /* Keys pressed since the last frame count as down for one frame (a quick shutter tap went down and up between two
       frames at ~12 fps, so Doom's menu never saw Enter) */
    u32 k = h->keys | h->latch;
    h->latch = 0;
    h->ctx = ((prog_t)h->code)(&svc, HX_FRAME, k, h->ctx);
    if (h->ctx) post_frame(); else h->running = 0;
    return 0;
}

static void key(u32 bit, int n, int *param)
{
    struct host *h = state();
    u32 a = (u32)param;
    if (!h || n < 1 || (a & 3) || !((a >= 0x100 && a < 0x10000) || (a >= 0x02000000 && a < 0x02200000))) return;
    if (*param == 0) { h->keys |= bit; h->latch |= bit; }
    else if (*param == 2) h->keys &= ~bit;
}
#define KEY(fn, bit) int fn(void *win, int n, int *param) { (void)win; key(bit, n, param); return 0; }
KEY(hx_ok,  HX_KEY_OK)
KEY(hx_k15, HX_KEY_UP)
KEY(hx_k16, HX_KEY_DOWN)
KEY(hx_k17, HX_KEY_LEFT)
KEY(hx_k18, HX_KEY_RIGHT)

#define CHAIN(fn, orig) int fn(void *win, int n, int *param) { struct host *h = state(); if (h) stop(h); return ((handler_t)(orig))(win, n, param); }
/* Back = what the game MENU's back handler does (0x0604A8CC: request mode 2 'main'; the mode change closes the windows), because the
   Games button now opens this window without the menu under it. Only on a press (n == 1, *param == 0), like the original. */
int hx_back(void *win, int n, int *param)
{
    (void)win;
    struct host *h = state();
    /* Event 0x19 is a SHORT press of the power button (no separate back button; a long hold sends 0x1A, the firmware's power-off,
       left alone). While a program runs it is a key for the program (Doom: Escape / menu). */
    if (h && h->running) { key(HX_KEY_BACK, n, param); return 0; }
    if (h) stop(h);
    u32 a = (u32)param;
    if (n == 1 && !(a & 3) && ((a >= 0x100 && a < 0x10000) || (a >= 0x02000000 && a < 0x02200000)) && *param == 0) fw_mode_request(2, 0);
    return 0;
}
CHAIN(hx_close,  0x0604CB38)
CHAIN(hx_cclose, 0x0604CACC)
