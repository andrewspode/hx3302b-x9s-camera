/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 Andrew Spode */
/* newlib system calls on the firmware (via the services table, set in hx_main). Files: the firmware's FatFs wrappers (4 open files at most,
   SD root); stdout/stderr go to DOOM.LOG on the card (opened lazily, closed after each write so a crash keeps it). Memory: _sbrk inside one
   arena taken from the firmware heap at start-up (all but HX_KEEP bytes of the largest block). */
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include "services.h"
#undef errno
static int errno;                         /* private: the _r wrappers copy it into the newlib context */
extern const struct hx_services *hx_svc;
typedef u32 (*largest_t)(void);
typedef int (*seek_t)(int fd, u32 off_lo, u32 off_hi, u32 flags);
#define FW_LARGEST ((largest_t)0x0602E040)
#define FW_SEEK    ((seek_t)0x0601D018)
#define HX_KEEP    (24 * 1024)            /* left in the firmware heap for its own use (file buffers, present) */
#define FD_BASE    3                      /* our fds = firmware fd + FD_BASE (0..2 are stdin/out/err) */

static char *arena, *brk_now, *arena_end;
/* All reads go through a 64-byte-aligned bounce buffer from the firmware heap: the SD driver INVALIDATES the data cache over the destination
   before its DMA, which on a buffer that is not cache-line aligned also discards neighbouring dirty data (Doom's zone headers: the second
   camera run failed with 'Z_Malloc: failed on allocation of 40 bytes' right after the first WAD read). */
#define BOUNCE 16384
#ifndef HX_ARENA
#define HX_ARENA (96 * 1024)                  /* flash-resident build: newlib heap (emulator: 64,536 bytes used after start-up); the log reports use */
#endif
static u8 *bounce;
void hx_progress(int step);                 /* hx_doom.c: loading bar */

/* Guard pages 0x0201C000-0x0203FFFF (36 x 4 KiB) between the file system's buffer page (0x0201B000) and Doom's zone (0x02040000): filled with a
   pattern and WRITTEN BACK to RAM (so a later change is a real overwrite, not a cached write discarded by an invalidate), checked after every
   file operation; a hit is logged as a bitmask of changed pages. */
#define GUARD     ((volatile u32 *)0x0201C000)
#define GUARD_PAGES 36
#define GUARD_N   (GUARD_PAGES * 1024)
#define GUARD_PAT(i) (0xA5000000u | (i))
typedef void (*wb_t)(void *, u32);
static const char *last_name = "";
static void guard_fill(void)
{
    for (u32 i = 0; i < GUARD_N; i++) GUARD[i] = GUARD_PAT(i);
    ((wb_t)0x02004524)((void *)GUARD, GUARD_N * 4);
}
static void log_write(const char *p, int n);
static void put_hex(char *o, u32 v) { for (int i = 7; i >= 0; i--) { o[i] = "0123456789ABCDEF"[v & 15]; v >>= 4; } }
#define HX_GUARD 0                                   /* set to 1 to check a range again */
static void guard_check(const char *op)
{
    static int busy;
    if (!HX_GUARD || busy) return;
    u32 lo = 0, hi = 0;                                         /* changed-page bitmask, pages 0-31 / 32-35 */
    for (u32 pg = 0; pg < GUARD_PAGES; pg++)
        for (u32 i = pg * 1024; i < pg * 1024 + 1024; i++)
            if (GUARD[i] != GUARD_PAT(i)) { if (pg < 32) lo |= 1u << pg; else hi |= 1u << (pg - 32); break; }
    if (!lo && !hi) return;
    char m[96]; int l = 0;
    const char *a = "GUARD "; while (*a) m[l++] = *a++;
    while (*op && l < 20) m[l++] = *op++;
    m[l++] = ' '; m[l++] = '\''; for (const char *q = last_name; *q && l < 50; q++) m[l++] = *q; m[l++] = '\''; m[l++] = ' ';
    put_hex(m + l, hi); l += 8; put_hex(m + l, lo); l += 8; m[l++] = '\n';
    busy = 1; log_write(m, l); busy = 0;
    guard_fill();
}
static u32 pos[4];                        /* file positions (the firmware wrapper has no tell) */

int hx_heap_init(const struct hx_services *s)
{
    u32 big = FW_LARGEST();
    if (big < HX_KEEP + 64 * 1024) return -1;              /* the program itself took most of the heap; Doom's zone is borrowed RAM */
    u32 n = (big - HX_KEEP) & ~0xFFFu;
    bounce = s->malloc(BOUNCE);                                  /* firmware malloc: 64-byte aligned, size a multiple of 64 */
    if (!bounce) return -1;
    n -= BOUNCE;
#ifdef HX_FLASH
    if (n > HX_ARENA) n = HX_ARENA;                              /* flash-resident build: the program is not in the heap; the rest of it is Doom's zone */
#endif
    arena = s->malloc(n);
    if (!arena) return -1;
    brk_now = arena; arena_end = arena + n;
    if (HX_GUARD) guard_fill();
    return 0;
}

#ifdef HX_FLASH
/* flash-resident build: Doom's main zone region = the firmware heap's largest block, less HX_KEEP for the firmware */
/* Plus a separate SMALL region for the zone's small allocations (patch 9's own rover): without it a 32 KiB texture failed with
   250 KB free in pieces of at most 26 KB, because the flash build's leftover regions are tiny (4-5 KiB), so mobjs and the like went into
   the main region and broke it up. */
#define HX_SMALL_REGION (128 * 1024)
static u8 *small_region;
u8 *hx_fw_zone(int *size)
{
    u32 big = FW_LARGEST();
    if (big < HX_KEEP + HX_SMALL_REGION + 128 * 1024) return 0;
    u32 n = (big - HX_KEEP - HX_SMALL_REGION) & ~0xFFFu;
    u8 *z = hx_svc->malloc(n);
    *size = z ? (int)n : 0;
    small_region = z ? hx_svc->malloc(HX_SMALL_REGION) : 0;
    return z;
}
u8 *hx_fw_small(int *size) { *size = small_region ? HX_SMALL_REGION : 0; return small_region; }
#endif
u32 hx_heap_used(void) { return (u32)(brk_now - arena); }
u32 hx_heap_size(void) { return (u32)(arena_end - arena); }

void *_sbrk(ptrdiff_t inc)
{
    if (!arena || brk_now + inc > arena_end || brk_now + inc < arena) { errno = ENOMEM; return (void *)-1; }
    char *p = brk_now; brk_now += inc; hx_svc->wdt_kick(); return p;
}

/* The log: writing a line to the card takes ~110 ms (every 'slow frame' was a log write, and the dips every ~2 s were the 50-frame line). So once Doom is running (hx_log_buffered, set after start-up) lines collect in RAM and go
   to the card when the buffer is 3/4 full, every LOG_EVERY ms, on 'hx330x step' lines (level changes: slow anyway), on quit and on a
   fatal error. Before the first write of a run, the previous run's DOOM.LOG is kept as DOOM.OLD (otherwise a crash log would be overwritten). */
#define LOG_BUF 8192
#ifdef HX_RELEASE
#define LOG_EVERY 0xFFFFFFFFu                       /* release: no periodic write; level changes, quit and errors still flush */
#else
#define LOG_EVERY 60000
#endif
static char lbuf[LOG_BUF];
static u32 lbn, l_last;
int hx_log_buffered;
static void keep_old_log(void)
{
    int in = hx_svc->f_open("DOOM.LOG", 1);
    if (in < 0) return;
    int out = bounce ? hx_svc->f_open("DOOM.OLD", 0xB) : -1;
    if (out >= 0) {
        int r;                                      /* sequential whole-buffer reads from offset 0: sector aligned, into the aligned bounce */
        while ((r = hx_svc->f_read(in, bounce, BOUNCE)) > 0) { hx_svc->f_write(out, bounce, (u32)r); hx_svc->wdt_kick(); if (r < BOUNCE) break; }
        hx_svc->f_close(out);
    }
    hx_svc->f_close(in);
}
void hx_log_flush(void)
{
    static int created;
    if (!lbn) return;
    char name[9]; name[0] = 'D'; name[1] = 'O'; name[2] = 'O'; name[3] = 'M'; name[4] = '.'; name[5] = 'L'; name[6] = 'O'; name[7] = 'G'; name[8] = 0;
    if (!created) keep_old_log();
    int fd = created ? hx_svc->f_open(name, 3) : hx_svc->f_open(name, 0xB);
    if (fd >= 0) {
        created = 1;
        FW_SEEK(fd, hx_svc->f_size(fd), 0, 0);
        hx_svc->f_write(fd, lbuf, lbn);
        hx_svc->f_close(fd);
    }
    lbn = 0; l_last = hx_svc->now_ms();
    guard_check("log");
}
static void log_write(const char *p, int n)
{
    static int lines;
    for (int i = 0; i < n; i++) if (p[i] == '\n') hx_progress(++lines);
    if (lbn + (u32)n > LOG_BUF) hx_log_flush();
    if ((u32)n > LOG_BUF) n = LOG_BUF;
    memcpy(lbuf + lbn, p, (u32)n); lbn += (u32)n;
    int step = n > 12 && !memcmp(p, "hx330x step", 11);
    if (!hx_log_buffered || step || lbn > LOG_BUF * 3 / 4 || hx_svc->now_ms() - l_last > LOG_EVERY) hx_log_flush();
}

int _open(const char *name, int flags, int mode)
{
    (void)mode;
    int m = (flags & 3) == 0 ? 1 : ((flags & 0x200 /* O_CREAT */) ? 0xB : 3);
    last_name = name;
    int fd = hx_svc->f_open(name, m);
    guard_check("open");
    if (fd < 0 || fd > 3) { errno = ENOENT; return -1; }
    pos[fd] = 0;
    return fd + FD_BASE;
}
int _close(int fd) { if (fd >= FD_BASE) { hx_svc->f_close(fd - FD_BASE); guard_check("close"); } return 0; }
/* Reads: ask the firmware only for WHOLE SECTORS starting at a SECTOR BOUNDARY, into the start of the 64-byte-aligned bounce buffer, then copy
   out what was asked. FatFs serves a mid-sector start by copying from its own buffer into the destination with the CPU and then DMAs the next
   sectors right after it; the SD driver invalidates the cache over that DMA target, which (not cache-line aligned) also discards the bytes
   just copied in: corrupted lumps (title drawn with broken columns) and crashes while loading. */
extern unsigned hx_prof[];
static int read_inner(int fd, void *buf, size_t n);
int _read(int fd, void *buf, size_t n)              /* (profiling: hx_prof[5] ms, [6] bytes, [7] calls) */
{
    u32 t = hx_svc->now_ms();
    int r = read_inner(fd, buf, n);
    hx_prof[5] += hx_svc->now_ms() - t; hx_prof[6] += r > 0 ? (u32)r : 0; hx_prof[7]++;
    return r;
}
static int read_inner(int fd, void *buf, size_t n)
{
    if (fd < FD_BASE) return 0;
    int f = fd - FD_BASE;
    u8 *d = buf; int total = 0;
    while (n) {
        u32 at = pos[f], aligned = at & ~511u, skip = at - aligned;
        u32 want = (skip + n + 511) & ~511u;
        if (want > BOUNCE) want = BOUNCE;
        if (FW_SEEK(f, aligned, 0, 0) < 0) return total ? total : -1;
        int r = hx_svc->f_read(f, bounce, want);
        guard_check("read");
        if (r <= (int)skip) break;                              /* error or end of file */
        u32 k = (u32)r - skip; if (k > n) k = n;
        memcpy(d, bounce + skip, k);
        d += k; n -= k; total += (int)k; pos[f] += k;
        hx_svc->wdt_kick();
    }
    return total;
}
int _write(int fd, const void *buf, size_t n)
{
    if (fd == 1 || fd == 2) { log_write(buf, (int)n); return (int)n; }
    if (fd < FD_BASE) return (int)n;
    int r = hx_svc->f_write(fd - FD_BASE, buf, n);
    guard_check("write");
    if (r > 0) pos[fd - FD_BASE] += r;
    return r < 0 ? -1 : r;
}
off_t _lseek(int fd, off_t off, int whence)
{
    if (fd < FD_BASE) return 0;
    int f = fd - FD_BASE;
    u32 target = whence == 0 ? (u32)off : whence == 1 ? pos[f] + (u32)off : hx_svc->f_size(f) + (u32)off;
    int sr = FW_SEEK(f, target, 0, 0);
    guard_check("seek");
    if (sr < 0) { errno = EINVAL; return -1; }
    pos[f] = target;
    return (off_t)target;
}
int _fstat(int fd, struct stat *st)
{
    st->st_mode = fd < FD_BASE ? S_IFCHR : S_IFREG;
    st->st_size = fd < FD_BASE ? 0 : (off_t)hx_svc->f_size(fd - FD_BASE);
    return 0;
}
int _isatty(int fd) { return fd < FD_BASE; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void hx_fatal(void);                        /* hx_doom.c: red screen */
void _exit(int code) { (void)code; hx_fatal(); for (;;) hx_svc->wdt_kick(); }   /* Doom's I_Error/I_Quit end here: red screen + hang; the log has the reason */
int _gettimeofday(struct timeval *tv, void *tz) { (void)tz; u32 ms = hx_svc->now_ms(); tv->tv_sec = ms / 1000; tv->tv_usec = (ms % 1000) * 1000; return 0; }
int _unlink(const char *name) { (void)name; errno = ENOENT; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = EMLINK; return -1; }
int _stat(const char *name, struct stat *st) { int fd = _open(name, 0, 0); if (fd < 0) return -1; _fstat(fd, st); _close(fd); return 0; }
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }

/* newlib for this target calls the re-entrant names directly (REENTRANT_SYSCALLS_PROVIDED): thin wrappers */
#include <reent.h>
void *_sbrk_r(struct _reent *r, ptrdiff_t n) { void *p = _sbrk(n); if (p == (void *)-1) r->_errno = errno; return p; }
int _open_r(struct _reent *r, const char *n, int f, int m) { int x = _open(n, f, m); if (x < 0) r->_errno = errno; return x; }
int _close_r(struct _reent *r, int fd) { (void)r; return _close(fd); }
_ssize_t _read_r(struct _reent *r, int fd, void *b, size_t n) { (void)r; return _read(fd, b, n); }
_ssize_t _write_r(struct _reent *r, int fd, const void *b, size_t n) { (void)r; return _write(fd, b, n); }
_off_t _lseek_r(struct _reent *r, int fd, _off_t o, int w) { _off_t x = _lseek(fd, o, w); if (x < 0) r->_errno = errno; return x; }
int _fstat_r(struct _reent *r, int fd, struct stat *st) { (void)r; return _fstat(fd, st); }
int _isatty_r(struct _reent *r, int fd) { (void)r; return _isatty(fd); }
int _getpid_r(struct _reent *r) { (void)r; return 1; }
int _kill_r(struct _reent *r, int p, int s) { r->_errno = EINVAL; (void)p; (void)s; return -1; }
int _unlink_r(struct _reent *r, const char *n) { (void)n; r->_errno = ENOENT; return -1; }
int _link_r(struct _reent *r, const char *a, const char *b) { (void)a; (void)b; r->_errno = EMLINK; return -1; }
int _rename_r(struct _reent *r, const char *a, const char *b) { (void)a; (void)b; r->_errno = ENOSYS; return -1; }
int _stat_r(struct _reent *r, const char *n, struct stat *st) { (void)r; return _stat(n, st); }
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) { (void)r; return _gettimeofday(tv, tz); }

/* newlib's OR1K port gets the current re-entrancy context from libgloss; single-threaded here: always the global one */
struct _reent *_or1k_libc_getreent(void) { return _impure_ptr; }

/* used by the zone region test: some real file system + SD activity (open the WAD, read 3 x 16 KiB at different offsets, close) */
int hx_io_stress(void)
{
    char n[10]; n[0]='D'; n[1]='O'; n[2]='O'; n[3]='M'; n[4]='1'; n[5]='.'; n[6]='W'; n[7]='A'; n[8]='D'; n[9]=0;
    int fd = hx_svc->f_open(n, 1);
    if (fd < 0) return -1;
    for (u32 k = 0; k < 3; k++) { FW_SEEK(fd, k * 0x123400u, 0, 0); hx_svc->f_read(fd, bounce, BOUNCE); }
    hx_svc->f_close(fd);
    return 0;
}
