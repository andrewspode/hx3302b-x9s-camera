/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* SD-card program loader, placed in the 'fly' game's code space (flash 0x4CBA0, runs in place at 0x0604CBA0).
   Every event in fly's window is logged to KEYLOG.BIN (rewritten after each event) and OK (0x14) / directions 0x15-0x18 still load
   GAME.BIN / GAME1..4.BIN when present. Fly's table entry for its timer (0x11) is relabelled 0x1A (power) by the update builder.
   Back (0x19), close (0x0A) and child close (0x0B) log, then chain to fly's own handlers. On a key press: load the file from the SD root into malloc'd RAM, write back the data cache, reset the instruction cache, call its first byte.
   No writable globals (this code runs from flash). Firmware functions are bound with --defsym in build.sh. */
#include "services.h"

int  fw_f_open(const char *name, int mode);     /* returns fd >= 0 */
u32  fw_f_size(int fd);
int  fw_f_read(int fd, void *buf, u32 len);     /* returns bytes read */
void fw_f_close(int fd);
int  fw_f_write(int fd, const void *buf, u32 len);   /* returns bytes written */
void *fw_malloc(u32);
void fw_free(void *);
void *fw_memcpy(void *, const void *, u32);
void fw_dcache_writeback(void *addr, u32 len);
void fw_icache_reset(void);
void fw_present(void);
u32  fw_now_ms(void);
void fw_wdt_kick(void);

static const struct hx_services svc = {
    HX_SERVICES_MAGIC, 1, fw_present, fw_now_ms, fw_wdt_kick, fw_memcpy, fw_malloc, fw_free,
    (u8 **)0x02019FF8, (u16 *)0x0201A1A4, (u16 *)0x0201A1A6,
};

int hx_stub(void *win, int n, int *param) { (void)win; (void)n; (void)param; return 0; }

/* Key log. The loader has no writable globals (it runs from flash), so the log buffer's pointer lives in fly's own game-buffer variable
   0x02019358, which fly's close handler frees and clears. Record = {ms, event, n, param[0], param pointer}; file = 'KEYL', count, records. */
#define LOG_PTR   ((u32 **)0x02019358)
#define LOG_MAGIC 0x4C59454Bu
#define LOG_MAX   1000u
typedef int (*handler_t)(void *, int, int *);
static int ram_ptr(const void *p) { u32 a = (u32)p; return a >= 0x02000000 && a < 0x02200000 && (a & 3) == 0; }
/* The event parameter pointer is NOT in main RAM: accept the 64 KiB SRAM at 0 as well */
static int param_ptr(const void *p) { u32 a = (u32)p; return (a & 3) == 0 && ((a >= 0x100 && a < 0x10000) || (a >= 0x02000000 && a < 0x02200000)); }
static void log_event(u32 id, int n, int *param)
{
    u32 *L = *LOG_PTR;
    if (!ram_ptr(L) || L[0] != LOG_MAGIC) return;
    u32 c = L[1];
    if (c < LOG_MAX) {
        u32 *r = L + 2 + c * 5;
        r[0] = fw_now_ms(); r[1] = id; r[2] = (u32)n;
        r[3] = (n >= 1 && param_ptr(param)) ? (u32)param[0] : 0xFFFFFFFFu;
        r[4] = (u32)param;                                           /* the raw pointer */
        L[1] = c + 1;
    }
    int fd = fw_f_open("KEYLOG.BIN", 0xB);
    if (fd >= 0) { fw_f_write(fd, L, 8 + L[1] * 20); fw_f_close(fd); }
}

static void run(const char *name)
{
    int fd = fw_f_open(name, 1);
    if (fd < 0) return;
    u32 size = fw_f_size(fd);
    void *buf = 0;
    if (size >= 4 && size <= 0x100000) buf = fw_malloc(size);
    if (buf && fw_f_read(fd, buf, size) != (int)size) { fw_free(buf); buf = 0; }
    fw_f_close(fd);
    if (!buf) return;
    fw_dcache_writeback(buf, size);
    fw_icache_reset();
    ((void (*)(const struct hx_services *))buf)(&svc);
    fw_free(buf);
}

/* keys: log, then on the press only (param 0) run the matching program if the file exists */
#define KEY(fn, id, file) int fn(void *win, int n, int *param) { (void)win; log_event(id, n, param); if (n == 1 && *param == 0) run(file); return 0; }
KEY(hx_ok,  0x14, "GAME.BIN")
KEY(hx_k15, 0x15, "GAME1.BIN")
KEY(hx_k16, 0x16, "GAME2.BIN")
KEY(hx_k17, 0x17, "GAME3.BIN")
KEY(hx_k18, 0x18, "GAME4.BIN")

/* window open: allocate the log (a stale pointer would only leak), then log */
int hx_open(void *win, int n, int *param)
{
    (void)win;
    u32 *L = fw_malloc(8 + LOG_MAX * 20);
    if (L) { L[0] = LOG_MAGIC; L[1] = 0; }
    *LOG_PTR = L;
    log_event(0x09, n, param);
    return 0;
}
/* power: log and swallow (does the window see it before the firmware's power-off handler?) */
int hx_power(void *win, int n, int *param) { (void)win; log_event(0x1A, n, param); return 0; }
/* back / close / child close: log, then fly's own handler (close frees the log) */
#define CHAIN(fn, id, orig) int fn(void *win, int n, int *param) { log_event(id, n, param); return ((handler_t)(orig))(win, n, param); }
CHAIN(hx_back,   0x19, 0x0604CAF8)
CHAIN(hx_close,  0x0A, 0x0604CB38)
CHAIN(hx_cclose, 0x0B, 0x0604CACC)
